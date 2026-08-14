ans = 123;
rng(42);
setter_semicolon_ok = ans == 123;
ans = 124;
rng(43)
setter_visible_ok = ans == 124;
summary = random_runtime_checks() + setter_semicolon_ok + setter_visible_ok

function summary = random_runtime_checks()
    rng(42);
    uniform_scalar = rand;
    uniform_square = rand(3);
    normal_cube = randn(2, 3, 2);

    rng(42);
    uniform_scalar_again = rand;
    uniform_square_again = rand(3);
    normal_cube_again = randn(2, 3, 2);
    reproducible_ok = isequal(uniform_scalar, uniform_scalar_again) && ...
                      isequal(uniform_square, uniform_square_again) && ...
                      isequal(normal_cube, normal_cube_again);

    checkpoint = rng;
    tail = randn(1, 5);
    rng(checkpoint);
    tail_again = randn(1, 5);
    state_restore_ok = isequal(tail, tail_again);

    shaped = rand([2 3 4]);
    empty_random = rand(2, 0, 3);
    shape_ok = isequal(size(shaped), [2 3 4]);
    empty_ok = isequal(size(empty_random), [2 0 3]) && ...
               numel(empty_random) == 0;

    single_random = rand(2, 3, 'single');
    single_like = rand(2, 3, 'like', single(0));
    class_ok = strcmp(class(single_random), 'single') && ...
               strcmp(class(single_like), 'single');

    rng(7);
    positive_integers = randi(10, 3);
    signed_integers = randi([-3 3], 2, 4);
    byte_integers = randi(255, [2 3], 'uint8');
    randi_shape_ok = isequal(size(positive_integers), [3 3]) && ...
                     isequal(size(signed_integers), [2 4]);
    randi_range_ok = all(positive_integers >= 1, 'all') && ...
                     all(positive_integers <= 10, 'all') && ...
                     all(signed_integers >= -3, 'all') && ...
                     all(signed_integers <= 3, 'all');
    randi_class_ok = strcmp(class(byte_integers), 'uint8') && ...
                     isequal(size(byte_integers), [2 3]);

    rng('default');
    default_value = rand;
    rng('default');
    default_ok = isequal(default_value, rand);
    finite_normal_ok = all(isfinite(normal_cube), 'all');

    summary = sum([reproducible_ok state_restore_ok shape_ok empty_ok ...
                   class_ok randi_shape_ok randi_range_ok randi_class_ok ...
                   default_ok finite_normal_ok]);
end
