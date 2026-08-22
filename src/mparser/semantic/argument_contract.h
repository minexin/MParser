#pragma once

#include "mparser/frontend/diagnostic.h"
#include "mparser/semantic/semantic.h"

#include <map>
#include <string>
#include <vector>

namespace mparser {

enum class ClassPropertyArgumentSetAccess {
    Public,
    Immutable,
    NonPublic,
};

struct ClassPropertyArgument {
    std::string name;
    std::string declaringClass;
    PropertySpec property;
    SourceSpan span;
    ClassPropertyArgumentSetAccess setAccess =
        ClassPropertyArgumentSetAccess::Public;
    bool constant = false;
};

struct ClassArgumentContract {
    std::string name;
    SourceSpan span;
    std::vector<std::string> superclasses;
    std::vector<ClassPropertyArgument> properties;
};

struct ArgumentContractCatalog {
    std::map<std::string, ClassArgumentContract> classes;
};

struct ResolvedArgumentContract {
    std::string name;
    PropertySpec property;
    SourceSpan span;
    ArgumentBlockKind blockKind = ArgumentBlockKind::Input;
    const HirNode* declaration = nullptr;
    bool classDerived = false;
};

struct ArgumentContractResolution {
    std::vector<ResolvedArgumentContract> contracts;
    std::vector<Diagnostic> diagnostics;
};

ArgumentContractCatalog buildArgumentContractCatalog(const HirNode& root);

ArgumentContractResolution resolveArgumentContracts(
    const HirNode& function, const ArgumentContractCatalog& catalog);

std::vector<Diagnostic> validateClassPropertyArgumentContracts(
    const HirNode& root, const ArgumentContractCatalog& catalog);

} // namespace mparser
