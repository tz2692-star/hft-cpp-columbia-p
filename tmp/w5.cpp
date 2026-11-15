#include <cstdio>
#include <iostream>

#include <vector>
#include <algorithm>
#include <numeric>

template <class T>
struct RingStat {
    std::vector<T> v;
    void push(T x) { v.push_back(x); }
    T    sum()  const { return std::accumulate(v.begin(), v.end(), T{}); }
    void sort()       { std::sort(v.begin(), v.end()); }
    T    median()     { sort(); return v[v.size() / 2]; }
};
int main() { 
    std::puts("session5 scratch");
    int i = max_of(3,4);
    float f = max_of(1.5, 2.5);
    float s = i + f;
    std::cout << s << std::endl;


    
    
    return 0; 
}
