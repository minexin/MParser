classdef Vault < handle
    properties (GetAccess = {?VaultReader}, SetAccess = {?VaultWriter})
        Value = 0
    end

    methods (Access = ?VaultFactory)
        function obj = Vault(seed)
            obj.Value = seed;
        end
    end

    methods (Access = {?VaultReader})
        function value = secretCode(obj)
            value = obj.Value + 100;
        end
    end
end

classdef VaultFactory
    methods (Static)
        function obj = create(seed)
            obj = Vault(seed);
        end
    end
end

classdef VaultReader
    methods (Static)
        function value = read(obj)
            value = obj.Value;
        end

        function value = secret(obj)
            value = obj.secretCode();
        end
    end
end

classdef PrivilegedReader < VaultReader
    methods (Static)
        function value = peek(obj)
            value = obj.Value;
        end
    end
end

classdef VaultWriter
    methods (Static)
        function write(obj, value)
            obj.Value = value;
        end
    end
end

classdef (AllowedSubclasses = ?ExtensionPoint) RestrictedRoot
    methods
        function value = code(obj)
            value = 23;
        end
    end
end

classdef ExtensionPoint < RestrictedRoot
end

classdef ExtensionLeaf < ExtensionPoint
end

vault = VaultFactory.create(7);
initial = VaultReader.read(vault);
secret = VaultReader.secret(vault);
VaultWriter.write(vault, 11);
updated = PrivilegedReader.peek(vault);

extension = ExtensionLeaf();
extension_code = extension.code();
