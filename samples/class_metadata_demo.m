classdef MetadataBase < handle
    properties
        BaseValue = 3
    end

    properties (Hidden)
        HiddenValue = 9
    end

    properties (GetAccess = private)
        SecretValue = 11
    end

    methods
        function value = baseMethod(obj)
            value = obj.BaseValue;
        end
    end

    methods (Hidden)
        function value = hiddenMethod(obj)
            value = obj.HiddenValue;
        end
    end

    events
        Changed
    end
end

classdef MetadataChild < MetadataBase
    properties
        ChildValue = 4
    end

    methods
        function value = childMethod(obj)
            value = obj.ChildValue;
        end
    end

    events (Hidden)
        HiddenChanged
    end
end

object = MetadataChild();
class_info = ?MetadataChild;
object_info = metaclass(object);
class_name = class_info.Name;
superclass_name = class_info.SuperclassList(1).Name;
first_property = class_info.PropertyList(1).Name;
first_event = class_info.EventList(1).Name;
same_class = class_info == object_info;
is_subclass = class_info < ?MetadataBase;
property_names = properties(object);
method_names = methods(object);
event_names = events(object);
property_count = numel(property_names);
method_count = numel(method_names);
event_count = numel(event_names);
has_base_value = isprop(object, 'BaseValue');
has_hidden_method = ismethod(object, 'hiddenMethod');
old_alias_matches = isa(class_info, 'meta.class');
lookup_info = matlab.metadata.Class.fromName('MetadataChild');
lookup_matches = lookup_info == class_info;
method_info = metafunction('MetadataChild/childMethod');
method_name = method_info.Name;
summary = property_count + method_count + event_count + same_class + ...
    is_subclass + has_base_value + old_alias_matches + lookup_matches;
