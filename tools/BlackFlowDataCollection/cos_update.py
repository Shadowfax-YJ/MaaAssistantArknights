"""Publish immutable BlackFlow packages to COS, then advance the CDN feeds."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import re
import time
from urllib.parse import urlsplit
from urllib.request import Request, urlopen

from updates import numeric_version, sha256, validate_advance


class CosStore:
    def __init__(self, client, bucket, service_error):
        self.client, self.bucket, self.service_error = client, bucket, service_error

    def _missing(self, error):
        return error.get_status_code() == 404 and error.get_error_code() == "NoSuchKey"

    def read(self, key):
        try:
            response = self.client.get_object(Bucket=self.bucket, Key=key)
        except self.service_error as error:
            if self._missing(error):
                return None
            raise
        stream = response["Body"].get_raw_stream()
        try:
            data = stream.read(2 * 1024 * 1024 + 1)
            if len(data) > 2 * 1024 * 1024:
                raise ValueError("Update feed is too large")
            return data
        finally:
            stream.close()

    def head(self, key):
        try:
            response = self.client.head_object(Bucket=self.bucket, Key=key)
        except self.service_error as error:
            # A HEAD response has no XML body; the SDK names an object 404 NoSuchResource.
            # preflight reads the feed first, so a missing bucket/denied GET has already failed.
            if error.get_status_code() == 404 and error.get_error_code() in ("NoSuchKey", "NoSuchResource"):
                return None
            raise
        return int(response["Content-Length"]), response.get("x-cos-meta-sha256", "")

    def upload(self, key, path, cache):
        self.client.upload_file(
            Bucket=self.bucket, Key=key, LocalFilePath=str(path), EnableMD5=True,
            CacheControl=cache,
            ContentType={".json": "application/json; charset=utf-8", ".xml": "application/xml; charset=utf-8",
                         ".txt": "text/plain; charset=utf-8"}.get(path.suffix, "application/octet-stream"),
            Metadata={"x-cos-meta-sha256": sha256(path)},
        )


def verify_public_file(url, path):
    """Exercise the same unauthenticated HTTPS download path as an installed client."""
    expected = sha256(path)
    digest, size = hashlib.sha256(), 0
    with urlopen(Request(url, headers={"User-Agent": "MAA-BlackFlow-Release", "Cache-Control": "no-cache"}), timeout=30) as response:
        if urlsplit(response.url).scheme != "https":
            raise ValueError("CDN download redirected to an insecure URL")
        while chunk := response.read(1024 * 1024):
            size += len(chunk)
            if size > path.stat().st_size:
                raise ValueError("CDN file is larger than the release artifact")
            digest.update(chunk)
    if size != path.stat().st_size or digest.hexdigest() != expected:
        raise ValueError("CDN content does not match the release artifact")


class CosPublisher:
    def __init__(self, config, store, purge, verify=verify_public_file):
        url = urlsplit(config["base_url"])
        prefix = config["prefix"]
        if (url.scheme != "https" or not url.hostname or url.username or url.password or url.query or url.fragment
                or url.path != "/" + prefix or not re.fullmatch(r"[a-zA-Z0-9_-]+(?:/[a-zA-Z0-9_-]+)*", prefix)
                or not re.fullmatch(r"[a-z0-9-]+-\d+", config["bucket"])
                or not re.fullmatch(r"[a-z]+-[a-z]+(?:-\d+)?", config["region"])):
            raise ValueError("Invalid BlackFlow COS/CDN configuration")
        self.config, self.store, self.purge, self.verify = config, store, purge, verify

    @classmethod
    def from_environment(cls, config):
        secret_id = os.environ.get("BLACKFLOW_COS_SECRET_ID", "")
        secret_key = os.environ.get("BLACKFLOW_COS_SECRET_KEY", "")
        if not secret_id or not secret_key:
            raise ValueError("Set BLACKFLOW_COS_SECRET_ID and BLACKFLOW_COS_SECRET_KEY in repository Actions secrets")
        from qcloud_cos import CosConfig, CosS3Client, CosServiceError
        from tencentcloud.common import credential
        from tencentcloud.cdn.v20180606 import cdn_client, models

        token = os.environ.get("BLACKFLOW_COS_SESSION_TOKEN") or None
        client = CosS3Client(CosConfig(Region=config["region"], SecretId=secret_id, SecretKey=secret_key, Token=token, Scheme="https"))
        cdn = cdn_client.CdnClient(credential.Credential(secret_id, secret_key, token), "")

        def purge(urls):
            request = models.PurgeUrlsCacheRequest()
            request.Urls = urls
            cdn.PurgeUrlsCache(request)

        return cls(config, CosStore(client, config["bucket"], CosServiceError), purge)

    def _key(self, relative):
        return self.config["prefix"] + "/" + relative

    def _url(self, relative):
        return self.config["base_url"] + "/" + relative

    def preflight(self, version, windows, macos, output):
        numeric_version(version)
        manifest = json.loads((output / "cdn/latest.json").read_text(encoding="utf-8"))
        if manifest["version"] != version or manifest["channel"] != "blackflow-data-collection" or manifest["schema_version"] != 1:
            raise ValueError("CDN feed version/channel mismatch")
        previous = self.store.read(self._key("latest.json"))
        if previous is not None:
            validate_advance(json.loads(previous), manifest)
        for platform, path in (("win-x64", windows), ("macos-universal", macos)):
            entry = manifest["assets"][platform]
            if (entry["name"] != path.name or entry["size"] != path.stat().st_size or entry["sha256"] != sha256(path)
                    or entry["url"] != self._url(f"{version}/{path.name}")):
                raise ValueError("CDN package does not match its generated feed")
        for path in (windows, macos, output / "SHA256.txt"):
            existing = self.store.head(self._key(f"{version}/{path.name}"))
            if existing is not None and existing != (path.stat().st_size, sha256(path)):
                raise ValueError("Refusing to replace an existing COS version with different bytes")

    def publish(self, version, windows, macos, output):
        self.preflight(version, windows, macos, output)
        packages = (windows, macos, output / "SHA256.txt")
        for path in packages:
            key = self._key(f"{version}/{path.name}")
            if self.store.head(key) is None:
                self.store.upload(key, path, "public, max-age=31536000, immutable")
            if self.store.head(key) != (path.stat().st_size, sha256(path)):
                raise ValueError("COS upload verification failed")
        # Clear any cached pre-release 404/403, and verify actual public bytes before exposing the version.
        self.purge([self._url(f"{version}/{path.name}") for path in packages])
        for path in packages:
            self._verify_with_retry(self._url(f"{version}/{path.name}"), path)
        for name in ("appcast.xml", "latest.json"):
            self.store.upload(self._key(name), output / "cdn" / name, "public, max-age=60, must-revalidate")
        self.purge([self._url(name) for name in ("appcast.xml", "latest.json")])
        for name in ("appcast.xml", "latest.json"):
            self._verify_with_retry(self._url(name), output / "cdn" / name)

    def _verify_with_retry(self, url, path):
        for attempt in range(13):
            try:
                self.verify(url, path)
                return
            except (OSError, ValueError):
                if attempt == 12:
                    raise
                time.sleep(10)
