function run_private_delete_reference
recorder = PrivateDeleteRecorder();
obj = PrivateDeleteOwner(recorder);
deleter = obj.makeDeleter();
result.release = version('-release');
result.named_rejected = rejects(@()delete(obj));
result.dot_rejected = rejects(@()obj.delete());
result.handle_rejected = rejects(@()external_handle(obj));
deleter();
result.alive = isvalid(obj);
result.count = recorder.Count;
disp(jsonencode(result));
assert(result.named_rejected && result.dot_rejected && ~result.handle_rejected);
assert(~result.alive && result.count == 1);

externalRecorder = PrivateDeleteRecorder();
externalObject = PrivateDeleteOwner(externalRecorder);
cleanup = externalObject.makeDeleter();
externalHandle = external_handle(externalObject);
result.external_handle_call_rejected = rejects(@()externalHandle());
result.external_alive = isvalid(externalObject);
result.external_count = externalRecorder.Count;
cleanup();

disp(jsonencode(result));
assert(result.external_handle_call_rejected);
assert(result.external_alive && result.external_count == 0);
assert(~isvalid(externalObject) && externalRecorder.Count == 1);
end

function result = rejects(action)
try
    action();
    result = false;
catch
    result = true;
end
end

function value = external_handle(obj)
value = @obj.delete;
end
