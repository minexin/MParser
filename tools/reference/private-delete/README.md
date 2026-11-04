# Private Destructor Reference

Run with MATLAB R2024b, not the MParser command line:

```matlab
addpath('D:/project/MParser/tools/reference/private-delete');
run_private_delete_reference;
```

## Observed Results

MATLAB `24.2.0.2712019 (R2024b)` produced this first-stage result:

```json
{"release":"2024b","named_rejected":true,"dot_rejected":true,"handle_rejected":false,"alive":false,"count":1}
```

Evidence: `build/private-delete-matlab-r2024b-diagnostic.log`. That run exited
nonzero because the original fixture incorrectly expected handle creation to
fail. The source now checks the measured result and prints it before attempting
the next independent probe.

Direct function/dot calls were rejected; external bound-handle creation was
accepted; a class-created bound handle deleted its object once. These results
alone do not establish what happens when an externally created handle is invoked.
The independent-object probe was subsequently executed successfully after
authorization and approval access were restored. Evidence:
`build/private-delete-matlab-r2024b-observed.log`. Its result additionally reports
`external_handle_call_rejected=true`, `external_alive=true`, `external_count=0`.
Calling the class-created cleanup handle afterwards deletes that object once.
The source asserts these measured results; earlier approval failures were not
successful MATLAB runs.

MParser currently rejects external private bound-handle creation. This is a
known compatibility difference. Do not describe that rejection as an R2024b
requirement or infer permission to invoke the private method from creation alone.
The invocation rejection matches the measured reference; the remaining difference
is its earlier rejection at handle creation.
