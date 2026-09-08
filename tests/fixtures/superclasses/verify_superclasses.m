leaf = HierarchyLeaf();
expected = {'HierarchyLeft'; 'HierarchyRoot'; 'handle'; 'HierarchyRight'};
assert(isequal(superclasses('HierarchyLeaf'), expected));
assert(isequal(superclasses(leaf), expected));
assert(isequal(superclasses([leaf, leaf]), expected));
assert(isequal(size(superclasses('double')), [0, 1]));
assert(isempty(superclasses(3)));
assert(isempty(superclasses('UnavailableClass')));
assert(isequal(superclasses(?HierarchyLeaf), ...
    {'matlab.metadata.MetaData'; 'handle'; 'matlab.mixin.Heterogeneous'}));
disp('superclasses fixture passed');
