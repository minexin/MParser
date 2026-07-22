char_value = 'alpha';
string_value = "alpha";
unicode_value = "A😀";

quote_classes = strcmp(class(char_value), 'char') && ...
                strcmp(class(string_value), 'string');
type_predicates = ischar(char_value) && isstring(string_value) && ...
                  isStringScalar(string_value);
empty_semantics = isempty('') && ~isempty("");
element_counts = numel(char_value) == 5 && numel(string_value) == 1;
lengths = strlength(char_value) == 5 && strlength(string_value) == 5;
unicode_units = strlength(unicode_value) == 3;

char_matrix = ['ab'; 'cd'];
char_shape = size(char_matrix, 1) == 2 && size(char_matrix, 2) == 2;
char_index = strcmp(char_value(2), 'l');
char_edit = 'abcd';
char_edit(2:3) = 'XY';
char_assignment = strcmp(char_edit, 'aXYd');
char_growth = 'a';
char_growth(3) = 'c';
char_growth_ok = strcmp(char_growth, 'a c');
char_delete = 'abcd';
char_delete(2:3) = [];
char_delete_ok = strcmp(char_delete, 'ad');

string_grid = ["one", "Two"; "three", "four"];
string_shape = size(string_grid, 1) == 2 && size(string_grid, 2) == 2;
string_index = strcmp(string_grid(2, 1), "three");
string_brace = strcmp(string_grid{1, 2}, 'Two');
string_grid(2, 2) = "FOUR";
string_assignment = strcmp(string_grid(2, 2), "FOUR");
string_grid{1, 1} = 'ONE';
string_brace_assignment = strcmp(string_grid(1, 1), "ONE");
string_append = string_grid + "!";
append_ok = strcmp(string_append(2, 2), "FOUR!");
expanded_append = ["a"; "b"] + ["1", "2"];
append_expansion_ok = size(expanded_append, 1) == 2 && ...
                      size(expanded_append, 2) == 2 && ...
                      strcmp(expanded_append(2, 2), "b2");
comparison_ok = all(string_grid == ...
    ["ONE", "Two"; "three", "FOUR"], "all");
expanded_match = ["a"; "b"] == ["a", "x"];
comparison_expansion_ok = expanded_match(1, 1) && ...
                          ~expanded_match(2, 1) && ...
                          ~expanded_match(1, 2) && ...
                          ~expanded_match(2, 2);
strcmpi_ok = strcmpi("Alpha", 'alpha');

codes = double('AZ');
numeric_conversion = codes(1) == 65 && codes(2) == 90;
code_matrix = double(char_matrix);
code_layout = code_matrix(2, 1) == 99 && code_matrix(1, 2) == 98;
coerced_codes = double(char([-1, 65.9, 70000]));
char_numeric_rules = coerced_codes(1) == 0 && ...
                     coerced_codes(2) == 65 && ...
                     coerced_codes(3) == 65535;
roundtrip = strcmp(char(codes), 'AZ');
row_strings = string(char_matrix);
matrix_conversion = size(row_strings, 1) == 2 && ...
                    all(strcmp(char(row_strings), char_matrix), "all");
length_grid = strlength(["a", "bb"; "ccc", "dddd"]);
length_layout = length_grid(2, 1) == 3 && length_grid(1, 2) == 2;

blank_strings = strings(2, 2);
blank_constructor = isstring(blank_strings) && ...
                    all(strlength(blank_strings) == 0, "all");
grown = strings(1, 1);
grown(3) = "tail";
growth_missing = ismissing(grown(2)) && strcmp(grown(3), "tail");
grown(2) = [];
string_delete = size(grown, 2) == 2 && strcmp(grown(2), "tail");

reshaped = reshape(string_grid, 1, 4);
reshape_ok = size(reshaped, 2) == 4 && strcmp(reshaped(2), "three");
repeated = repmat("x", 2, 3);
repmat_ok = all(repeated == "x", "all");
transposed = string_grid';
transpose_ok = strcmp(transposed(2, 1), "Two");
text_cells = cellstr(char_matrix);
cellstr_ok = strcmp(text_cells{2}, 'cd');
string_cells = cellstr(["a", "b"; "c", "d"]);
cellstr_string_layout = strcmp(string_cells{2, 1}, 'c') && ...
                        strcmp(string_cells{1, 2}, 'b');
concatenated = horzcat("a", "b");
concat_ok = size(concatenated, 2) == 2 && strcmp(concatenated(2), "b");

summary = sum([quote_classes, type_predicates, empty_semantics, ...
    element_counts, lengths, unicode_units, char_shape, char_index, ...
    char_assignment, char_growth_ok, char_delete_ok, string_shape, ...
    string_index, string_brace, string_assignment, ...
    string_brace_assignment, append_ok, append_expansion_ok, comparison_ok, ...
    comparison_expansion_ok, strcmpi_ok, numeric_conversion, code_layout, ...
    char_numeric_rules, roundtrip, matrix_conversion, length_layout, ...
    blank_constructor, ...
    growth_missing, string_delete, reshape_ok, repmat_ok, transpose_ok, ...
    cellstr_ok, cellstr_string_layout, concat_ok]);
