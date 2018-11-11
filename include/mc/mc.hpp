#pragma once

#include <cstdint>
#include <functional>
#include <string_view>
#include <tuple>

namespace mc {

    struct nil_t {};

    template <typename>
    struct tuple_tail;

    template <typename ...Tail>
    struct tuple_tail <std::tuple<Tail...>> {
        using type = std::tuple<>;
    };

    template <typename Head, typename ...Tail>
    struct tuple_tail <std::tuple<Head, Tail...>> {
        using type = std::tuple<Tail...>;
    };


    template<typename T>
    struct meta;
    
    template<typename Descriptor>
    struct Module {};

    template<typename Descriptor>
    struct Namespace {};

    template<typename Descriptor>
    struct Enum {
        using descriptor = Descriptor;

        constexpr bool in_range(int value) const {
            using enumerators_tuple = typename descriptor::enumerators;
            return std::apply([value] (auto ...enums) {
                return (... || (enums.value == value));
            }, enumerators_tuple());
        }

        template <typename visitorT>
        constexpr void for_each_enumerator(visitorT visitor) const {
            using enumerators_tuple = typename descriptor::enumerators;
            std::apply([visitor] (auto ...enums) {
                (visitor(enums), ...);
            }, enumerators_tuple());
        }
    };

    template<typename Descriptor>
    struct Enumerator {};

    template<typename Descriptor>
    struct Parameter {};

    template<typename Descriptor>
    struct Method {
        using descriptor = Descriptor;

        template <typename ...Args>
        constexpr auto operator()(Args&& ...args) {

        }
    };

    template<typename Descriptor>
    struct OverloadSet {
        using descriptor = Descriptor;
        constexpr std::string_view get_name() const {
            return descriptor::name;
        }

        constexpr bool operator==(const std::string_view nm) const {
            return get_name() == nm;
        }
    };

    template<typename Descriptor>
    struct Operator {};

    template<typename Descriptor>
    struct ConstructorSet {};

    template<typename Descriptor>
    struct Field {};

    template<typename Descriptor>
    struct Class {
        using descriptor = Descriptor;
        constexpr bool has_method(std::string_view methodName) const {
            using methods_tuple = typename descriptor::methods;
            return std::apply([methodName] (auto ...meth) {
                return (... || (meth.name == methodName));
            }, methods_tuple());
        }


        // This needs to be aware of overloads but for now let's just get the ball rolling with a simplistic approach
        template <typename tuple_t>
        constexpr auto find_method(const std::string_view method_name) const {

            using methods_tuple = tuple_t;

            if constexpr (std::tuple_size<methods_tuple>::value == 0) {
                return mc::nil_t();
            }

            using head_type = typename std::tuple_element<0, methods_tuple>::type;
            // return head_type();
            if constexpr(head_type() == method_name) {
                return head_type();
            } else {
                return find_method<typename tuple_tail<methods_tuple>::type>(method_name);
            }
        }
    };


    class DynamicClass {
    public:
        virtual ~DynamicClass() = 0;
        virtual bool hasMethod(std::string_view name) const = 0;
        virtual void call(std::string_view method, const void *obj, int argc, void **argv) const = 0;
        // virtual void call(std::string_view method, void *obj, int argc, void **argv) = 0;
    private:
    };
    DynamicClass::~DynamicClass() = default;

    template <typename WrappedType>
    class DynamicClassWrapper : public DynamicClass, public mc::meta<WrappedType>{
        using descriptor = mc::meta<WrappedType>;

        inline virtual ~DynamicClassWrapper() = default;
        inline virtual bool hasMethod(std::string_view name) const {
            return descriptor::has_method(name);
        }

        inline virtual void call(std::string_view method, const void *obj, int argc, void **argv) const {
            const auto& Object = *static_cast<const typename descriptor::type*>(obj);
        }

    };

}
