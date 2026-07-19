function summary = exception_runtime_demo()
try
    error("MParserDemo:BadValue", "Value %d for %s", 7, "item");
catch caught
    caught_id = caught.identifier;
    caught_message = caught.message;
    caught_class = class(caught);
    caught_line = caught.stack.line;
    caught_stack_is_struct = isstruct(caught.stack);
    caught_cause_is_empty = isempty(caught.cause);
end

manual = MException("MParserDemo:Manual", "Manual %s", "failure");
try
    throw(manual);
catch thrown
    throw_line = thrown.stack.line;
    try
        rethrow(thrown);
    catch reraised
        rethrow_line = reraised.stack.line;
    end
end

try
    missing_runtime_name;
catch automatic
    automatic_id = automatic.identifier;
end

id_ok = strcmp(caught_id, "MParserDemo:BadValue");
message_ok = strcmp(caught_message, "Value 7 for item");
class_ok = strcmp(caught_class, "MException");
isa_ok = isa(caught, "MException");
line_ok = caught_line > 0;
preserved_ok = throw_line == rethrow_line;
automatic_ok = strcmp(automatic_id, "MParser:RuntimeError");

summary = id_ok * 100000000 + message_ok * 10000000 + ...
          class_ok * 1000000 + isa_ok * 100000 + line_ok * 10000 + ...
          caught_stack_is_struct * 1000 + caught_cause_is_empty * 100 + ...
          preserved_ok * 10 + automatic_ok;
end
