# File Receipt Acknowledgement - Test Results

Date: 2026-10-09
Environment: CentOS VM, loopback connection 127.0.0.1:10990.

## Automated verification
Ran tests/file_receipt_test.py on the VM.
Result: ALL 16 FILE RECEIPT TEST GROUPS PASSED.

## Manual verification
- Private transfer: Bob received and saved the 31-byte receipt_demo.txt.
- Alice received Bob's SAVED acknowledgement.
- Private summary: saved=1 failed=0 unknown=0.
- cmp confirmed that the server and Bob copies matched the original.
- Server log contained FILE_RECEIPT and FILE_COMPLETE records.
- Controlled save failure: a regular file blocked the receive directory.
- Alice received failuser's SAVE_FAILED acknowledgement.
- Failure summary: saved=0 failed=1 unknown=0.
- failuser successfully ran LIST after the failed save.
- Room receiptlab: Bob reported SAVED and failuser reported SAVE_FAILED.
- Room summary: saved=1 failed=1 unknown=0.
