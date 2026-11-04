# Bound Method Access Reference

Run `run_method_handle_access_reference` with this directory on the MATLAB path.
The R2024b run at `build/method-handle-access-matlab-r2024b-final.log` reports
MATLAB `24.2.0.2712019`, public result 7, authorised private/protected results
31/41, and `MATLAB:class:MethodRestricted` for the two externally created handles.
It exits successfully with all assertions enabled.

Both handles can be created outside their class. Their later invocation is
rejected, including invocation through a public method of the owner class.
Class-created private and derived-class-created protected handles remain callable
outside the class. Call counters remain one each, proving that the rejected
attempts did not execute the method bodies.

This reference covers non-static bound method handles. It does not establish
class-qualified/static handle syntax, arbitrary unregistered module binding,
complete class precedence or every MATLAB access-control rule.
