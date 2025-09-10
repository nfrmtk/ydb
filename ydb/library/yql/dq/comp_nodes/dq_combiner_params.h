#pragma once
#include <yql/essentials/minikql/mkql_node.h>
namespace NKikimr {
namespace NMiniKQL {
template<typename T>
TRuntimeNode MakeRuntimeNode(T value){
    auto* ptr = new TValueNode<T>{ std::move(value)};
    return TRuntimeNode{ptr, false};
};
struct TCombinerParams{
    int foo;
    double baz;
}; 


}
}