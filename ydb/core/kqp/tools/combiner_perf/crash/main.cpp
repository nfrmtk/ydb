#include <iostream>

int * g(){
    return new int(5);
}

int* f(){
    int* p = g();
    int* p2 = p;
    delete p2;
    return p;
}

int main(){
    *f() = 3;
}