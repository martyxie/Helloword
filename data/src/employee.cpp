
#include <iostream>
#include "employee.h"

using namespace std;

namespace Records{
	Employee::Employee(){
		mFirstName("");
		mLastName = "";
		mEployeeNumber(-1);
		mSalary(DefalueStartSalary);
		mHired = false;
	}

	void Employee::promote(int raiseAmount)
	{
		setSalary(getSalary() + raiseAmount);
		return;
	}

	void Employee::demote(int demoteAmount)
	{
		setSalary(getSalary() - demoteAmount);
		return;
	}
	void Employee::setFirstName(const std::string &firstName)
	{
		mFirstName = firstName;
		return;
	}
	const string &getFirstName()
	{
		return mFirstName;
	}


}
