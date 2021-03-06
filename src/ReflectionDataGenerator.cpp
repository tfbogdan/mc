#include "ReflectionDataGenerator.h"

#include <unordered_map>
#include <string_view>
#include <map>
#include <fmt/format.h>
#include <fmt/ostream.h>
#include <experimental/filesystem>
#include <clang/Basic/OperatorKinds.h>
#include <clang/Basic/SourceManager.h>

#include <iostream>

#pragma warning(push, 0)
#pragma warning(pop)

extern llvm::cl::opt<std::string> mcOutput;
extern llvm::cl::opt<std::string> mcJsonOutput;

namespace mc {
    namespace fs = std::experimental::filesystem;


    ReflectionDataGenerator::ReflectionDataGenerator(clang::ASTContext &astContext, clang::Sema &Sema)
        :out(mcOutput),
        idman(astContext.getPrintingPolicy()),
        idrepo(),
        context(astContext),
        sema(Sema),
        printingPolicy(astContext.getPrintingPolicy()) {

        global_scope.putline("#pragma once");
        global_scope.putline("#include <array>");
        global_scope.putline("#include <string_view>");

        global_scope.putline("#include <rosewood/rosewood.hpp>");
        global_scope.putline("#include <rosewood/type.hpp>");

        auto mainFile = astContext.getSourceManager().getMainFileID();
        auto mainFileLoc = astContext.getSourceManager().getComposedLoc(mainFile, 0);
        auto mainFilePath = astContext.getSourceManager().getFilename(mainFileLoc);

        global_scope.putline("#include \"{}\"", mainFilePath.str());
        global_scope.putline("");
        global_scope.putline("namespace rosewood {{");
    }

    ReflectionDataGenerator::~ReflectionDataGenerator() {
        for(const auto &[declName, descriptorName]: exportedMetaTypes) {
            global_scope.putline("template <>");
            global_scope.putline("struct meta <{}> : public {} {{}};", declName, descriptorName);
        }

        global_scope.putline("}}");
        idrepo.save(mcJsonOutput.getValue());
        out.flush();
    }

    template <typename declRangeT>
    void put_decl_range(scope where, const declRangeT &range, std::string_view separator = ",\n") {
        const std::size_t numElements = std::size(range);
        std::size_t idx(0);

        for(const auto& item: range) {
            where.indent();

            if constexpr (std::is_same<typename declRangeT::value_type, std::string>::value) {
                where.rawput(item);
            } else {
                auto itemName = item->getNameAsString();
                where.rawput("meta_{}", itemName);
            }

            if (idx < (numElements - 1)) {
                where.rawput(separator);
            }

            ++idx;
        }

    }

    template <typename declRangeT>
    void wrap_range_in_tuple(std::string_view withName, scope where, const declRangeT &range) {
        const std::size_t numElements = std::size(range);
        if (numElements == 0) {
            where.putline("using {} = std::tuple<>;", withName);
            return;
        }

        where.putline("using {} = std::tuple<", withName);
        auto initScope = where.spawn();

        std::size_t idx(0);
        for(const auto& item: range) {
            initScope.indent();

            if constexpr (std::is_same<typename declRangeT::value_type, std::string>::value || std::is_same<typename declRangeT::value_type, std::string_view>::value) {
                initScope.rawput(item);
            } else {
                auto itemName = item->getNameAsString();
                initScope.rawput("meta_{}", itemName);
            }

            if (idx < (numElements - 1)) {
                initScope.rawput(",");
            }

            initScope.rawput("\n");
            ++idx;
        }
        where.putline(">;");
    }

    clang::QualType ReflectionDataGenerator::getUnitType(clang::QualType T) {
        auto type = T.getTypePtr();

        auto isNotUnitType = [] (auto type) {
            return type->isPointerType() || type->isReferenceType();
        };

        while (isNotUnitType(type)) {
            type = type->getPointeeType().getTypePtr();
        }

        return clang::QualType(type, 0);
    }


    void ReflectionDataGenerator::Generate() {
        printingPolicy = context.getPrintingPolicy();
        descriptor_scope module_scope = descriptor_scope(global_scope.spawn(), mcModuleName.getValue(), "rosewood::Module");

        std::vector<std::string> exportedNamespaces;
        std::vector<std::string> exportedEnums;
        std::vector<std::string> exportedClasses;

        for(const auto decl: context.getTranslationUnitDecl()->decls()) {
            // first cull out everything that isn't defined within the `main` file
            const bool inMainFile(context.getSourceManager().isInMainFile(decl->getLocation()));
            if (inMainFile) {
                switch(auto declKind = decl->getKind()) {
                case clang::Decl::Kind::Namespace:
                    exportedNamespaces.push_back(fmt::format("meta_{}", exportNamespace(static_cast<const clang::NamespaceDecl*>(decl), module_scope).name));
                    break;
                case clang::Decl::Kind::Enum:
                    exportedEnums.push_back(fmt::format("meta_{}", exportEnum(static_cast<const clang::EnumDecl*>(decl), module_scope).name));
                    break;
                case clang::Decl::Kind::CXXRecord: {
                    auto record = static_cast<const clang::CXXRecordDecl*>(decl);
                    if (record->isThisDeclarationADefinition()) {
                        exportedClasses.push_back(fmt::format("meta_{}", exportCxxRecord(record->getNameAsString(), record, module_scope).name));
                    }
                } break;
                case clang::Decl::Kind::ClassTemplateSpecialization: {
                    // auto specialization = static_cast<clang::ClassTemplateSpecializationDecl*>(decl);
                    // this is just a test. it's not expected to work due to template naming
                    // if(specialization->isThisDeclarationADefinition()) {
                    //    exportedClasses.push_back(fmt::format("meta_{}", exportCxxRecord(specialization, module_scope).name));
                    // }
                } break;
                default:
                    // report?
                    break;
                }
            }


        }
        wrap_range_in_tuple("namespaces", module_scope.inner, exportedNamespaces);
        wrap_range_in_tuple("enums", module_scope.inner, exportedEnums);
        wrap_range_in_tuple("classes", module_scope.inner, exportedClasses);
        // and another range to rule them all
        std::vector<std::string_view> all_decls;
        all_decls.insert(all_decls.end(), exportedNamespaces.begin(), exportedNamespaces.end());
        all_decls.insert(all_decls.end(), exportedEnums.begin(), exportedEnums.end());
        all_decls.insert(all_decls.end(), exportedClasses.begin(), exportedClasses.end());

        wrap_range_in_tuple("declarations", module_scope.inner, all_decls);
    }

    descriptor_scope ReflectionDataGenerator::exportDeclaration(const clang::Decl *Decl, descriptor_scope &where) {
        // so this is a sort of a type dispatcher
        switch(auto declKind = Decl->getKind()) {
        case clang::Decl::Kind::Namespace:
            return exportNamespace(static_cast<const clang::NamespaceDecl*>(Decl), where);
        case clang::Decl::Kind::Enum:
            return exportEnum(static_cast<const clang::EnumDecl*>(Decl), where);
        case clang::Decl::Kind::CXXRecord: {
            auto record = static_cast<const clang::CXXRecordDecl*>(Decl);
            if (record->isThisDeclarationADefinition()) {
                return exportCxxRecord(record->getNameAsString(), record, where);
            }
        } break;
        default:
            break;
        }
        // TDO: Once we have better coverage of declaration types, this should become a throw
        return where.spawn("", "");
    }

    void ReflectionDataGenerator::exportType(const std::string &exportAs, clang::QualType type, descriptor_scope &where) {
        const auto canonicalTypeName = type.getCanonicalType().getAsString(printingPolicy);
        const auto plainTypeName = type.getAsString(printingPolicy);
        const auto atomicTypeName = getUnitType(type).getCanonicalType().getAsString(printingPolicy);
        where.putline("static constexpr rosewood::Type<{0}> {3}{{\"{1}\", \"{0}\", \"{2}\"}};", canonicalTypeName, plainTypeName, atomicTypeName, exportAs);
    }

    bool isNoExcept(const clang::CXXMethodDecl *method) {
        bool isIt = false;
        auto functionPrototype = method->getType()->getAs<clang::FunctionProtoType>();
        auto exceptionSpecifier = functionPrototype->getExceptionSpecType();

        switch (exceptionSpecifier) {
            case clang::ExceptionSpecificationType::EST_NoexceptTrue:
                [[fallthrough]];
            case clang::ExceptionSpecificationType::EST_BasicNoexcept:
                isIt = true;
            break;
            default:
            break;
        }
        return isIt;
    }

    bool isKnownNoExcept(const clang::CXXMethodDecl *method) {
        bool isIt = false;
        auto functionPrototype = method->getType()->getAs<clang::FunctionProtoType>();
        auto exceptionSpecifier = functionPrototype->getExceptionSpecType();

        switch (exceptionSpecifier) {
            case clang::ExceptionSpecificationType::EST_NoexceptTrue:
                [[fallthrough]];
            case clang::ExceptionSpecificationType::EST_BasicNoexcept:
                [[fallthrough]];
            case clang::ExceptionSpecificationType::EST_NoexceptFalse:
                return true;
            default:
            break;
        }
        return false;
    }


    std::string ReflectionDataGenerator::buildMethodSignature(const clang::CXXMethodDecl *method) {
        // build the argument list
        llvm::SmallVector<char, 1024> buff;
        llvm::raw_svector_ostream sstream(buff);
        auto functionPrototype = method->getType()->getAs<clang::FunctionProtoType>();
        bool noExcept = isNoExcept(method);

        sstream << method->getReturnType().getCanonicalType().getAsString(printingPolicy);
        sstream << fmt::format(" ({}::*) (", clang::QualType(method->getParent()->getTypeForDecl(), 0).getAsString(printingPolicy));
        if (method->parameters().empty()) {

        } else {
            int paramIdx = 0;
            for (const auto& parm: method->parameters()) {
                sstream << parm->getType().getCanonicalType().getAsString(printingPolicy);
                sstream << (paramIdx < (method->parameters().size() - 1) ? ",": "");
                ++paramIdx;
            }
        }
        std::string_view noexceptness = isKnownNoExcept(method) ? (noExcept ? " noexcept": " noexcept(false)") : "";
        sstream << ")";
        sstream << fmt::format("{}{}",
                               functionPrototype->isConst() ? " const" : "",
                               noexceptness);

        auto res = sstream.str().str();
        return res;
    }

    void ReflectionDataGenerator::exportMethods(const clang::CXXRecordDecl *Record, const std::vector<const clang::CXXMethodDecl*> &methods, descriptor_scope &outerScope) {

        int methodIndex = 0;

        outerScope.putline("static constexpr std::tuple methods {{");
        ++outerScope.inner;
        for(const auto Method: methods) {
            const bool isLast = methodIndex == (methods.size() - 1);

            outerScope.putline("rosewood::MethodDeclaration {{");
            ++outerScope.inner;

            outerScope.putline(fmt::format("static_cast<{}>(&{}),", buildMethodSignature(Method), Method->getQualifiedNameAsString()));
            outerScope.putline(fmt::format("\"{}\",", Method->getNameAsString()));
            if (Method->parameters().empty()) {
                outerScope.putline("std::tuple{{}}}}{}", isLast ? "" : ",");
            } else {
                int paramIdx = 0;
                outerScope.putline("std::tuple{{");
                ++outerScope.inner;
                for (const auto& param: Method->parameters()) {
                    outerScope.putline(fmt::format("rosewood::FunctionParameter<{}>(\"{}\", {}, {}){}",
                                                   param->getType().getCanonicalType().getAsString(printingPolicy),
                                                   param->getNameAsString(),
                                                   param->hasDefaultArg(),
                                                   paramIdx,
                                                   paramIdx < (Method->parameters().size() - 1) ? ",": ""
                                                   ));
                    ++paramIdx;
                }
                --outerScope.inner;
                outerScope.putline("}}}}{}", isLast ? "" : ",");
            }
            --outerScope.inner;
            ++methodIndex;
        }
        --outerScope.inner;
        outerScope.putline("}};");
    }

    void ReflectionDataGenerator::exportConstructors(const std::vector<const clang::CXXConstructorDecl*> &ctors, const clang::CXXRecordDecl *record, descriptor_scope &outerScope) {
        int methodIndex = 0;

        outerScope.putline("static constexpr std::tuple constructors {{");
        ++outerScope.inner;
        for(const auto ctor: ctors) {
            const bool isLast = methodIndex == (ctors.size() - 1);
            const bool noExcept = isNoExcept(ctor);
            outerScope.inner.put("rosewood::ConstructorDeclaration<{}, {}", clang::QualType(record->getTypeForDecl(), 0).getAsString(printingPolicy), noExcept);

            if (ctor->parameters().empty()) {
                outerScope.inner.rawput("> {{\n");
            } else {
                int paramIdx = 0;
                outerScope.inner.rawput(", ");
                for (const auto& param: ctor->parameters()) {
                    outerScope.inner.rawput("{}{}", param->getType().getCanonicalType().getAsString(printingPolicy), paramIdx < (ctor->parameters().size() - 1) ? ",": "");
                    ++paramIdx;
                }
                outerScope.inner.rawput("> {{\n");
            }

            ++outerScope.inner;

            if (ctor->parameters().empty()) {
                outerScope.putline("std::tuple{{}}}}{}", isLast ? "" : ",");
            } else {
                int paramIdx = 0;
                outerScope.putline("std::tuple{{");
                ++outerScope.inner;
                for (const auto& param: ctor->parameters()) {
                    outerScope.putline(fmt::format("rosewood::FunctionParameter<{}>(\"{}\", {}, {}){}",
                                                   param->getType().getCanonicalType().getAsString(printingPolicy),
                                                   param->getNameAsString(),
                                                   param->hasDefaultArg(),
                                                   paramIdx,
                                                   paramIdx < (ctor->parameters().size() - 1) ? ",": ""
                                                   ));
                    ++paramIdx;
                }
                --outerScope.inner;
                outerScope.putline("}}}}{}", isLast ? "" : ",");
            }
            --outerScope.inner;
            ++methodIndex;
        }
        --outerScope.inner;
        outerScope.putline("}};");
    }

    void ReflectionDataGenerator::exportFields(const std::vector<const clang::FieldDecl*> &fields, descriptor_scope &outerScope) {
        outerScope.put("static constexpr std::tuple fields {{");
        if (fields.empty()) {
            outerScope.inner.rawput("}};\n");
        } else {
            outerScope.inner.rawput("\n");
            ++outerScope.inner;
            int fIndex = 0;
            const char* prefix = " ";
            for(const auto& field: fields) {
                outerScope.putline("{0} rosewood::FieldDeclaration<{1}, {2}>{{\"{3}\", &{2}::{3}, {4}, offsetof({2}, {3})}}",
                                   std::exchange(prefix, ","),
                                   field->getType().getCanonicalType().getAsString(printingPolicy),
                                   clang::QualType(field->getParent()->getTypeForDecl(), 0).getAsString(printingPolicy),
                                   field->getNameAsString(),
                                   fIndex++);
            }
            --outerScope.inner;
            outerScope.inner.putline("}};");
        }
    }

    bool ReflectionDataGenerator::areMethodArgumentsPubliclyUsable(const clang::CXXMethodDecl* method) {
        for (const auto& param: method->parameters()) {
            auto parmType = param->getType();
            if (parmType->isRecordType()) {
                auto record = parmType->getAsRecordDecl();
                if (!record->getAccess() == clang::AccessSpecifier::AS_public) {
                    return false;
                }
            } // this will likely need to be extended to cover more cases
        }
        auto functionPrototype = method->getType()->getAs<clang::FunctionProtoType>();
        auto exceptionSpecKind = functionPrototype->getExceptionSpecType();
        // cannot export a function if we cannot reliably determine it's exception specification
        // since that's part of the c++17 type system
        return true;
        return  exceptionSpecKind != clang::ExceptionSpecificationType::EST_Uninstantiated &&
                exceptionSpecKind != clang::ExceptionSpecificationType::EST_DependentNoexcept &&
                exceptionSpecKind != clang::ExceptionSpecificationType::EST_Unevaluated;
    }


    descriptor_scope ReflectionDataGenerator::exportCxxRecord(const std::string &name, const clang::CXXRecordDecl *Record, descriptor_scope &where) {

        auto ownScope = where.spawn(name, "rosewood::StaticClass");
        ownScope.putline("using type = {};", clang::QualType(Record->getTypeForDecl(), 0).getAsString(printingPolicy));
        ownScope.putline("static constexpr std::string_view qualified_name = \"{}\";", Record->getQualifiedNameAsString());
        std::map<std::string_view, std::set<std::string>> descriptornames = {
            {"classes", {}},
            {"enums", {}}
        };

        std::vector<const clang::CXXConstructorDecl*> constructors;
        std::vector<const clang::CXXConversionDecl*> conversions;
        std::vector<const clang::FieldDecl*> fields;
        std::vector<const clang::CXXRecordDecl*> classes;
        std::vector<const clang::EnumDecl*> enums;

        std::vector<const clang::CXXMethodDecl*> exportedMethods;

        // std::vector<const clang::Decl*> decls;
        const clang::CXXDestructorDecl *destructor = nullptr;

        for(const auto decl: Record->decls()) {
            // first trim out non public members
            if(decl->getAccess() != clang::AccessSpecifier::AS_public) continue;

            switch (decl->getKind()) {
            case clang::Decl::Kind::CXXMethod: {
                auto method = static_cast<const clang::CXXMethodDecl*>(decl);
                if (!areMethodArgumentsPubliclyUsable(method)) continue;
                if (!method->isStatic() && !method->isDeleted()) {
                    exportedMethods.push_back(method);
                }
            } break;
            case clang::Decl::Kind::CXXConstructor: /*{
                auto ctor = static_cast<const clang::CXXConstructorDecl*>(decl);
                if (!areMethodArgumentsPubliclyUsable(ctor)) continue;
                if(!ctor->isDeleted()) {
                    constructors.push_back(ctor);
                }
            } */ break;
            case clang::Decl::Kind::CXXDestructor: {
                destructor = static_cast<const clang::CXXDestructorDecl*>(decl);
            } break;
            case clang::Decl::Kind::CXXConversion: {
                auto conv = static_cast<const clang::CXXConversionDecl*>(decl);
                if (!areMethodArgumentsPubliclyUsable(conv)) continue;
                conversions.push_back(conv);
            } break;
            case clang::Decl::Kind::Field: {
                auto field = static_cast<const clang::FieldDecl*>(decl);
                fields.push_back(field);
            } break;
            case clang::Decl::Kind::CXXRecord: {
                auto cls = static_cast<const clang::CXXRecordDecl*>(decl);
                if (cls->isThisDeclarationADefinition()) {
                    classes.push_back(cls);
                }
            } break;
            case clang::Decl::Kind::Enum: {
                auto en = static_cast<const clang::EnumDecl*>(decl);
                if (en->isThisDeclarationADefinition()) {
                    enums.push_back(en);
                }
            } break;

            default:
                break;
            }
        }

        for (const auto& ctor: Record->ctors()) {
            if (!areMethodArgumentsPubliclyUsable(ctor)) continue;
            if(!ctor->isDeleted()) {
                constructors.push_back(ctor);
            }
        }

        ownScope.putline("using bases_t = std::tuple < ");
        auto prefix = "";
        for (const auto &base: Record->bases()) {
            if (base.getAccessSpecifier() == clang::AccessSpecifier::AS_public) {
                ownScope.putline("{} {}", std::exchange(prefix, ","), base.getType().getCanonicalType().getAsString(printingPolicy));
            }
        }
        ownScope.putline(">;");

        exportConstructors(constructors, Record, ownScope);
        exportMethods(Record, exportedMethods, ownScope);
        exportFields(fields, ownScope);

        if (destructor != nullptr) {
            // exportCxxDestructor(destructor, Record, ownScope);
        }

        for(const auto cls: classes) {
            auto exportedScope = exportCxxRecord(cls->getNameAsString(), cls, ownScope);
            descriptornames["classes"].emplace(fmt::format("meta_{}", exportedScope.name));
        }
        for(const auto en: enums) {
            auto exportedScope = exportEnum(en, ownScope);
            descriptornames["enums"].emplace(fmt::format("meta_{}", exportedScope.name));
        }

        std::vector<std::string_view> all_decls;
        for(const auto& [rangeName, range]: descriptornames) {
            wrap_range_in_tuple(rangeName, ownScope.inner, range);
            all_decls.insert(all_decls.end(), range.begin(), range.end());

        }
        wrap_range_in_tuple("declarations", ownScope.inner, all_decls);

        exportedMetaTypes.emplace_back(std::tuple(clang::QualType(Record->getTypeForDecl(),0).getAsString(printingPolicy), ownScope.qualifiedName));
        return ownScope;
    }

    descriptor_scope ReflectionDataGenerator::exportEnum(const clang::EnumDecl *Enum, descriptor_scope &where) {
        auto qualName = Enum->getQualifiedNameAsString();
        auto name = Enum->getNameAsString();
        if (name.empty()) {
            name = Enum->getTypedefNameForAnonDecl()->getNameAsString();
            qualName = Enum->getTypedefNameForAnonDecl()->getQualifiedNameAsString();
        }

        auto ownScope = where.spawn(name, "rosewood::Enum");
        ownScope.putline("using type = {};", qualName);
        ownScope.putline("using enumerator_type = Enumerator<{}>;", Enum->getIntegerType().getAsString(printingPolicy));
        std::vector<clang::EnumConstantDecl*> enumerators(Enum->enumerators().begin(), Enum->enumerators().end());
        ownScope.putline("static constexpr std::array<Enumerator<{}>, {}> enumerators {{", Enum->getIntegerType().getAsString(printingPolicy), enumerators.size());

        for(unsigned index(0); index < enumerators.size(); ++index) {
            auto enumerator = enumerators[index];
            auto enName = enumerator->getNameAsString();
            auto enScope = ownScope.inner.spawn();
            enScope.putline("Enumerator<{}> {{ {}, \"{}\" }}{}", Enum->getIntegerType().getAsString(printingPolicy), enumerator->getInitVal().toString(10), enName, index < (enumerators.size() - 1) ? ",": std::string());
        }

        ownScope.putline("}};");
        exportedMetaTypes.emplace_back(std::tuple(qualName, ownScope.qualifiedName));
        return ownScope;
    }

    descriptor_scope ReflectionDataGenerator::exportNamespace(const clang::NamespaceDecl *Namespace, descriptor_scope &where) {
        auto qualName = Namespace->getQualifiedNameAsString();
        auto name = Namespace->getNameAsString();
        auto ownScope = where.spawn(name, "rosewood::Namespace");

        ownScope.print_header();

        std::vector<std::string> exportedNamespaces;
        std::vector<std::string> exportedEnums;
        std::vector<std::string> exportedClasses;

        for(const auto decl: Namespace->decls()) {
            switch(auto declKind = decl->getKind()) {
            case clang::Decl::Kind::Namespace:
                exportedNamespaces.push_back(fmt::format("meta_{}", exportNamespace(static_cast<const clang::NamespaceDecl*>(decl), ownScope).name));
                break;
            case clang::Decl::Kind::Enum:
                exportedEnums.push_back(fmt::format("meta_{}", exportEnum(static_cast<const clang::EnumDecl*>(decl), ownScope).name));
                break;
            case clang::Decl::Kind::CXXRecord: {
                auto record = static_cast<const clang::CXXRecordDecl*>(decl);
                if (record->isThisDeclarationADefinition()) {
                    exportedClasses.push_back(fmt::format("meta_{}", exportCxxRecord(record->getNameAsString()  , record, ownScope).name));
                }
            } break;
            case clang::Decl::TypeAlias: {
                auto alias = static_cast<clang::TypeAliasDecl*>(decl);
                auto aliasedType = alias->getUnderlyingType();
                if (aliasedType->isRecordType()) {
                    auto record = aliasedType->getAsCXXRecordDecl();
                    if (record->getKind() == clang::Decl::Kind::ClassTemplateSpecialization) {
                        auto specialization = static_cast<clang::ClassTemplateSpecializationDecl*>(record);
                        sema.RequireCompleteType(alias->getLocation(), clang::QualType(specialization->getTypeForDecl(), 0), 1);
                        exportedClasses.push_back(fmt::format("meta_{}", exportCxxRecord(alias->getNameAsString(), specialization->getDefinition(), ownScope).name));
                    }
                }
            };
            default:
                break;
            }
        }
        wrap_range_in_tuple("namespaces", ownScope.inner, exportedNamespaces);
        wrap_range_in_tuple("enums", ownScope.inner, exportedEnums);
        wrap_range_in_tuple("classes", ownScope.inner, exportedClasses);


        std::vector<std::string_view> all_decls;
        all_decls.insert(all_decls.end(), exportedNamespaces.begin(), exportedNamespaces.end());
        all_decls.insert(all_decls.end(), exportedEnums.begin(), exportedEnums.end());
        all_decls.insert(all_decls.end(), exportedClasses.begin(), exportedClasses.end());

        wrap_range_in_tuple("declarations", ownScope.inner, all_decls);

        return ownScope;
    }

}
