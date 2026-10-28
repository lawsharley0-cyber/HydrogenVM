import unittest
from unittest.mock import patch, MagicMock
import sys
import os

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), '..')))
import clash_royale_probe

class TestClashRoyaleProbe(unittest.TestCase):
    @patch('urllib.request.urlopen')
    @patch('urllib.request.Request')
    def test_downloader_path(self, mock_request, mock_urlopen):
        mock_response = MagicMock()
        mock_response.read.side_effect = [b"dummy data", b""]
        mock_urlopen.return_value.__enter__.return_value = mock_response

        with patch('hashlib.file_digest') as mock_hash:
            mock_hash.return_value.hexdigest.return_value = clash_royale_probe.SHA256

            with patch('pathlib.Path.open') as mock_open:
                with patch('sys.exit') as mock_exit:
                    with patch('zipfile.ZipFile') as mock_zip:
                        import argparse
                        with patch('argparse.ArgumentParser.parse_args') as mock_args:
                            args = argparse.Namespace(out="test_out", ipa=None)
                            mock_args.return_value = args

                            try:
                                clash_royale_probe.main()
                            except SystemExit:
                                pass

        mock_request.assert_called_once()
        mock_urlopen.assert_called_once()

if __name__ == '__main__':
    unittest.main()
