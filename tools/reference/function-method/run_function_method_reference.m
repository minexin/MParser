function run_function_method_reference
obj = DispatchOwner();
result.version = version;
result.release = version('-release');
result.direct = read(obj);
result.second = combine(4,obj);
[result.a,result.b] = parts(obj);
touch(obj);
result.changed = read(obj);
result.overridden = length(obj);
child = DispatchChild();
peer = DispatchPeer();
result.inherited = read(child);
result.allowed = inside(child);
result.left = pick(child,peer);
result.right = pick(peer,child);
result.mixed = pick(4,child,peer);
result.private_rejected = rejects(@()secret(obj));
result.numeric_rejected = rejects(@()read(4));
result.dominant_missing_rejected = rejects(@()read(peer,obj));
result.local = local_control(obj);
result.shadowed = shadow_control(obj);
deleted = DispatchOwner();
delete(deleted);
result.deleted_rejected = rejects(@()marker(4,deleted));
result.deleted_first = pick(deleted);
assert(isequal([result.direct,result.second,result.a,result.b,result.changed,result.overridden], ...
    [7,11,7,8,8,17]));
assert(isequal([result.inherited,result.allowed,result.left,result.right,result.mixed], ...
    [7,31,1,2,1]));
assert(result.private_rejected && result.numeric_rejected && result.dominant_missing_rejected);
assert(result.local == 91 && result.shadowed == 55);
assert(~result.deleted_rejected && result.deleted_first == 1);
disp(jsonencode(result));
end

function value = rejects(action)
try
    action();
    value = false;
catch
    value = true;
end
end

function value = local_control(obj)
value = read(obj);
    function y = read(obj)
        y = 91;
    end
end

function value = shadow_control(obj)
read = @(x)55;
value = read(obj);
end
