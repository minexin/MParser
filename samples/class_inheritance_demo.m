classdef Entity < handle
    properties
        Id
    end

    methods
        function setId(obj, id)
            obj.Id = id;
        end

        function value = describe(obj)
            value = obj.Id;
        end

        function value = score(obj)
            value = obj.Id;
        end
    end

    methods (Static)
        function value = category()
            value = 10;
        end
    end
end

classdef CounterEntity < Entity
    properties
        Count
    end

    methods
        function obj = CounterEntity(id, count)
            obj.Id = id;
            obj.Count = count;
        end

        function value = score(obj)
            value = obj.Id + obj.Count;
        end
    end
end

item = CounterEntity(3, 4);
alias = item;
alias.setId(8);
inherited_value = item.describe();
override_value = item.score();
static_value = CounterEntity.category();
base_property = item.Id;
derived_property = item.Count;
