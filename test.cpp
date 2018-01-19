#include<iostream>
#include<stdio.h>
#include<pthread.h>

using namespace std;
void *test_thread(void *arg);


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
//using namespace MyFirstnamespace;
using namespace MySecondnamespace;

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
	pthread_t pid;
		int open_thread(void* handle)
		{
			ret = pthread_create(&pid, NULL, test_thread, NULL);
			return 0;
		}

	protected:
		char c;

	private:
		int ret;
};

void *test_thread(void *arg)
{

	while(1)
	{
		cout << "test thrad " <<endl;
		sleep(1);
	}
	return  NULL;
};


class funtion{
	public:
		funtion(void){ //构造函数,名称要与，类名称一致
			cout << "this is no argumen" << endl;
		}
		funtion(int x){
			cout << "this is the one argument x = " << x << endl;
			m_x = x;
		}
		funtion(int x,int y){
			cout << "this is the  two argumen x= "<< x << "y= "<<y << endl;
			m_x = x;
			m_y = y;
		}
		void print(void){
			cout << " m_x = "<< m_x << "m_y= "<< m_y << endl;
		}

	protected:
		int a;

	private:
		int m_x;
		int m_y;
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
	cout << "-----------------------------------------"<<endl;
	funtion test;
	test.print();
	funtion test2(22);
	test2.print();
	funtion test3 (45,129);
	test3.print();

	return 0;
}
