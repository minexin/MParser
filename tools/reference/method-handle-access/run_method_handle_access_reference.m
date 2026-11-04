function run_method_handle_access_reference
owner = AccessOwner();
child = AccessChild();
publicHandle = @owner.publicRead;
privateHandle = @owner.privateRead;
protectedHandle = @child.protectedRead;
authorisedPrivate = owner.privateFactory();
authorisedProtected = child.protectedFactory();

result.release = version('-release');
result.public = publicHandle();
result.authorised_private = authorisedPrivate();
result.authorised_protected = authorisedProtected();
[result.private_rejected,result.private_identifier] = rejects(privateHandle);
[result.protected_rejected,result.protected_identifier] = rejects(protectedHandle);
result.private_via_owner_rejected = rejects(@()owner.invoke(privateHandle));
result.protected_via_owner_rejected = rejects(@()owner.invoke(protectedHandle));
result.private_calls = owner.PrivateCalls;
result.protected_calls = child.ProtectedCalls;
disp(jsonencode(result));
assert(result.public == 7 && result.authorised_private == 31 && result.authorised_protected == 41);
assert(result.private_rejected && result.protected_rejected);
assert(result.private_via_owner_rejected && result.protected_via_owner_rejected);
assert(result.private_calls == 1 && result.protected_calls == 1);
end

function [rejected,identifier] = rejects(action)
try
    action();
    rejected = false;
    identifier = '';
catch exception
    rejected = true;
    identifier = exception.identifier;
end
end
