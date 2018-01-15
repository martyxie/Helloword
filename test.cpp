#include<iostream>

using namespace std;


namespace MyFirstnamespace{
	int a;
	void cout_t(void){
		cout << "this is my first name space" << a << endl;
	}
}
namespace MySecondnamespace{
	int a;
	void cout_t(void){
		cout << "this is my secount name space" << a <<endl;
	}
}
using namespace MyFirstnamespace;

int main(void)
{

	cout << "hello this is my test c++" << endl;
	MyFirstnamespace::a = 3;
	MySecondnamespace::a = 100;
	MyFirstnamespace::cout_t();
	MySecondnamespace::cout_t();
	a = 102;
	cout_t();
	return 0;
}
