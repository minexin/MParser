dynamicParents = superclasses('dynamicprops');
metadataParents = superclasses('meta.DynamicProperty');
numericParents = superclasses('double');
disp(dynamicParents);
disp(metadataParents);
summary = numel(dynamicParents) + numel(metadataParents) + numel(numericParents);
