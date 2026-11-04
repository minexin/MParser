function run_private_delete_reference
recorder = PrivateDeleteRecorder();
obj = PrivateDeleteOwner(recorder);
deleter = obj.makeDeleter();
result.release = version('-release');
result.named_rejected = rejects(@()delete(obj));
result.dot_rejected = rejects(@()obj.delete());
result.handle_rejected = rejects(@()forbidden_handle(obj));
deleter();
result.alive = isvalid(obj);
result.count = recorder.Count;
assert(result.named_rejected && result.dot_rejected && result.handle_rejected);
assert(~result.alive && result.count == 1);
disp(jsonencode(result));
end

function result = rejects(action)
try
    action();
    result = false;
catch
    result = true;
end
end

function value = forbidden_handle(obj)
value = @obj.delete;
end
