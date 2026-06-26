int square(int x){ return x*x; }
int sum_to(int n){ int s=0; for(int i=1;i<=n;i++) s+=i; return s; }
unsigned fib(unsigned n){ unsigned a=0,b=1; while(n--){ unsigned t=a+b; a=b; b=t; } return a; }
