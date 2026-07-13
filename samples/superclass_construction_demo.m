classdef Report < RecordBase & RevisionBase
    properties
        Pages
    end
    methods
        function obj = Report(id, revision, pages)
            obj@RecordBase(id);
            obj = obj@RevisionBase(revision);
            obj.Pages = pages;
        end
        function value = score(obj)
            value = score@RecordBase(obj) + obj.Pages;
        end
        function value = adjustment(obj)
            value = obj.Pages;
        end
    end
end

classdef RecordBase
    properties
        Id
    end
    methods
        function obj = RecordBase(id)
            obj.Id = id;
        end
        function value = score(obj)
            value = obj.Id * 10 + obj.adjustment();
        end
        function value = adjustment(obj)
            value = 0;
        end
    end
end

classdef RevisionBase
    properties
        Revision
    end
    methods
        function obj = RevisionBase(revision)
            obj.Revision = revision;
        end
    end
end

classdef ForwardedRecord < RecordBase
end

classdef TaggedRecord < DefaultTag
    properties
        Payload
    end
    methods
        function obj = TaggedRecord(payload)
            obj.Payload = payload;
        end
    end
end

classdef DefaultTag
    properties
        Tag
    end
    methods
        function obj = DefaultTag()
            obj.Tag = 5;
        end
    end
end

report = Report(4, 2, 3);
qualified_score = report.score();
base_id = report.Id;
base_revision = report.Revision;
derived_pages = report.Pages;

forwarded = ForwardedRecord(7);
forwarded_id = forwarded.Id;

tagged = TaggedRecord(9);
implicit_tag = tagged.Tag;
tagged_payload = tagged.Payload;
