#include<iostream>
#include<stdio.h>
#include<pthread.h>

using namespace std;


namespace MyFirstnamespace{
	int a;
	void cout_t(void){
		cout << "this is my first name space " << a << endl;
	}
}
namespace MySecondnamespace{
	int a;
	void cout_t(void){
		cout << "this is my secount name space " << a <<endl;
	}
	void test_1(void){
	};
}
using namespace MyFirstnamespace;

class MyBook{
	public:
		int get_size(void);
		int set_size(int x,int y);

	protected:
		int hig; //长
		int wih; //宽
		static int test;
};

int MyBook::test = 0;

int MyBook::get_size(void)
{
	return hig*wih;
}

int MyBook::set_size(int x,int y)
{
	hig= x;
	wih= y;
	test = x+y;

	return 0;
}

class Myc:public MyBook{
	public:
		int get_all_size(void);
		int set_all_size(int min){
			this->h = min;
		}

	private:
		int h;
};

int Myc::get_all_size(void)
{
	//return get_size()*h;
	return wih*hig*h;
}


class Thread{

	public:
		int open_thread(void* handle)
		{
			ret = pthread_create(&pid, NULL, (void*)test_thread, NULL);
			return 0;
		}

	protected:
		char c;

	private:
		int ret;
		pthread_t pid;
		void *test_thread(void *arg)
		{
			while(1)
			{
				cout << "test thrad " <<endl;
				sleep(1);
			}
			return  NULL;
		}
};

int main(void)
{

	char *p = new char[1024];
	MyBook *book = new MyBook;
	cout << "hello this is my test c++" << endl;
	MyFirstnamespace::a = 3;
	MySecondnamespace::a = 100;
	MyFirstnamespace::cout_t();
	MySecondnamespace::cout_t();
	a = 102;
	cout_t();
	//memcpy(p,10101,1024);
	p[0] = 'c';  
	cout << p << endl;
	book->set_size(100,2);
	Myc cidian;
	cidian.set_size(12,34);
	cidian.set_all_size(2);
	cout << cidian.get_size() << endl;
	cout << cidian.get_all_size() << endl;
	cout << book->get_size() << endl;

	delete book;
	delete p;
	return 0;
}
