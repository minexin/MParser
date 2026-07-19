function summary = exception_diagnostics_demo()
try
    leaf_failure();
catch root
    wrapped = MException("MParserDemo:Wrapper", "wrapper failure");
    wrapped = wrapped.addCause(root);
    try
        throw(wrapped);
    catch captured
        basic_report = captured.getReport("basic");
        extended_report = captured.getReport("extended", "hyperlinks", "off");
    end
end

warning("MParserDemo:Notice", "continuing after warning %d", 7);
[warning_message, warning_id] = lastwarn();

try
    assert(0, "MParserDemo:Assertion", "assertion value %d", 9);
catch asserted
end

wrapper_ok = strcmp(captured.identifier, "MParserDemo:Wrapper");
cause_ok = numel(captured.cause) == 1;
basic_report_ok = strcmp(basic_report, "wrapper failure");
extended_report_ok = ~isempty(extended_report);
warning_ok = strcmp(warning_message, "continuing after warning 7") && ...
             strcmp(warning_id, "MParserDemo:Notice");
assert_ok = strcmp(asserted.identifier, "MParserDemo:Assertion") && ...
            strcmp(asserted.message, "assertion value 9");

summary = wrapper_ok * 100000 + cause_ok * 10000 + ...
          basic_report_ok * 1000 + extended_report_ok * 100 + ...
          warning_ok * 10 + assert_ok;
end

function leaf_failure()
error("MParserDemo:Root", "root failure");
end
