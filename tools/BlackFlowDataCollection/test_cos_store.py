"""Exercise request headers and HTTP failures through Tencent's actual SDK."""

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


if __name__ == "__main__":
    unittest.main()
