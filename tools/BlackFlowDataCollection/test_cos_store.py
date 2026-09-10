"""Exercise request headers and HTTP failures through Tencent's actual SDK."""

import base64
import hashlib
import tempfile
import unittest
from pathlib import Path
from unittest.mock import Mock, patch

from cos_update import CosPublisher, CosStore
from updates import CDN_CONFIG, sha256

try:
    from qcloud_cos import CosConfig, CosS3Client, CosServiceError
except ImportError:
    CosS3Client = None


@unittest.skipIf(CosS3Client is None, "Install COS SDK to run transport checks")
class CosSdkTests(unittest.TestCase):
    def setUp(self):
        self.client = CosS3Client(CosConfig(Region="ap-shanghai", SecretId="fixture-id", SecretKey="fixture-key", Scheme="https"))
        self.store = CosStore(self.client, CDN_CONFIG["bucket"], CosServiceError)

    def test_head_object_missing_sdk_error(self):
        error = CosServiceError("HEAD", {"code": "NoSuchResource"}, 404)
        with patch.object(self.client, "head_object", side_effect=error):
            self.assertIsNone(self.store.head("maa/blackflow/v1.2.3/package.zip"))

    def test_permission_error_and_missing_bucket_propagate(self):
        for code, status in (("AccessDenied", 403), ("NoSuchBucket", 404)):
            error = CosServiceError("GET", {"code": code}, status)
            with patch.object(self.client, "get_object", side_effect=error):
                with self.assertRaises(CosServiceError):
                    self.store.read("maa/blackflow/latest.json")

    def test_upload_uses_real_sdk_headers(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "latest.json"
            path.write_text('{"test": true}')
            response = Mock()
            response.headers = {"ETag": "fixture"}
            with patch.object(self.client, "send_request", return_value=response) as request:
                self.store.upload("maa/blackflow/latest.json", path, "public, max-age=60, must-revalidate")
            headers = request.call_args.kwargs["headers"]
            self.assertEqual(headers["x-cos-meta-sha256"], sha256(path))
            self.assertIn("max-age=60", headers["Cache-Control"])
            self.assertTrue(headers["Content-Type"].startswith("application/json"))
            self.assertIn("Content-MD5", headers)

    def test_clients_construct_without_network_or_secret_output(self):
        with patch.dict("os.environ", {"BLACKFLOW_COS_SECRET_ID": "fixture-id", "BLACKFLOW_COS_SECRET_KEY": "fixture-key"}, clear=True):
            publisher = CosPublisher.from_environment(CDN_CONFIG)
        self.assertEqual(publisher.store.bucket, "lubiao-wiki-1450633361")
        self.assertEqual(
            publisher.store.client._conf.uri(bucket=CDN_CONFIG["bucket"]),
            "https://lubiao-wiki-1450633361.cos.ap-shanghai.tencentcos.cn/",
        )

    def test_multipart_uses_object_requests_and_preserves_headers_and_bytes(self):
        requests, uploaded = [], []

        def transport(**request):
            requests.append(request)
            params = request.get("params", {})
            response = Mock()
            response.headers = {}
            if request["method"] == "POST" and "uploads" in params:
                response.content = b"<InitiateMultipartUploadResult><UploadId>fixture-upload</UploadId></InitiateMultipartUploadResult>"
            elif request["method"] == "PUT" and "partNumber" in params:
                chunk = request["data"]
                if hasattr(chunk, "read"):
                    chunk = chunk.read()
                uploaded.append(chunk)
                self.assertEqual(request["headers"]["Content-MD5"], base64.b64encode(hashlib.md5(chunk).digest()))
                response.headers = {"ETag": hashlib.md5(chunk).hexdigest()}
            elif request["method"] == "POST" and "uploadId" in params:
                response.content = b"<CompleteMultipartUploadResult><ETag>fixture</ETag></CompleteMultipartUploadResult>"
            else:
                self.fail(f"Unexpected COS request: {request['method']} {params}")
            return response

        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "package.zip"
            contents = b"a" * (1024 * 1024) + b"b" * 1024
            path.write_bytes(contents)
            with patch.object(self.client, "send_request", side_effect=transport):
                self.store.upload("maa/blackflow/v1.2.3/package.zip", path, "public, max-age=31536000, immutable")
            self.assertEqual(b"".join(uploaded), contents)
            self.assertEqual([len(chunk) for chunk in uploaded], [1024 * 1024, 1024])
            self.assertEqual(requests[0]["headers"]["x-cos-meta-sha256"], sha256(path))
            self.assertIn("immutable", requests[0]["headers"]["Cache-Control"])
            self.assertEqual(len(requests), 4)

    def test_multipart_failure_aborts_own_upload_and_preserves_original_error(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "package.zip"
            path.write_bytes(b"a" * (2 * 1024 * 1024))
            failure = RuntimeError("part upload failed")
            for cleanup_failure in (None, RuntimeError("cleanup failed")):
                with patch.object(self.client, "create_multipart_upload", return_value={"UploadId": "our-upload"}), \
                        patch.object(self.client, "upload_part", side_effect=failure), \
                        patch.object(self.client, "complete_multipart_upload") as complete, \
                        patch.object(self.client, "abort_multipart_upload", side_effect=cleanup_failure) as abort:
                    with self.assertRaises(RuntimeError) as raised:
                        self.store.upload("maa/blackflow/v1.2.3/package.zip", path, "immutable")
                    self.assertIs(raised.exception, failure)
                    complete.assert_not_called()
                    abort.assert_called_once_with(Bucket=CDN_CONFIG["bucket"], Key="maa/blackflow/v1.2.3/package.zip", UploadId="our-upload")

    def test_sdk_rewinds_part_stream_after_partial_network_send(self):
        from requests import ConnectionError

        calls, uploaded = [], []

        def send(url, **request):
            body = request["data"]
            calls.append(int(request["params"]["partNumber"]))
            if len(calls) == 1:
                self.assertEqual(body.read(65536), b"a" * 65536)
                raise ConnectionError("simulated disconnect after a partial send")
            data = body.read()
            uploaded.append(data)
            self.assertEqual(request["headers"]["Content-MD5"], base64.b64encode(hashlib.md5(data).digest()))
            response = Mock(status_code=200)
            response.headers = {"ETag": hashlib.md5(data).hexdigest()}
            return response

        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "package.zip"
            contents = b"a" * (1024 * 1024) + b"b" * 1024
            path.write_bytes(contents)
            with patch.object(self.client, "create_multipart_upload", return_value={"UploadId": "our-upload"}), \
                    patch.object(self.client, "complete_multipart_upload") as complete, \
                    patch.object(self.client, "abort_multipart_upload") as abort, \
                    patch.object(self.client._session, "put", side_effect=send), \
                    patch("qcloud_cos.cos_client.time.sleep"):
                self.store.upload("maa/blackflow/v1.2.3/package.zip", path, "immutable")
            self.assertEqual(calls, [1, 1, 2])
            self.assertEqual(b"".join(uploaded), contents)
            self.assertEqual(len(complete.call_args.kwargs["MultipartUpload"]["Part"]), 2)
            abort.assert_not_called()


if __name__ == "__main__":
    unittest.main()
