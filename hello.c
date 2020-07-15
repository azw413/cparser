

int add(int a, int b)
{
    return a + b;
}


int test()
{
    let a = "A test";
    let b = 4.0;
    let c = true; //add(1, 9) * 2.0;
    let d = add;
    //let e = { 0, 1, 2, 3 };
    return b + c;
}

int main(int argc, char * argv[])
{
   printf("Hello %d\n", test());
   return 0;
}
