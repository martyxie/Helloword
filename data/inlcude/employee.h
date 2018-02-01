#pragma once
#include <string>
namespace Records{

	const int DefalueStartSalary = 3000;
	class Employee{
		public:
			Employee();
			void promote(int raiseAmout = 1000);
			void demote(int demeritAmount = 1000);
			void hire();
			void fire();
			void display() const;
			//设置雇员的名称
			void setFirstName(const std::string &firstName);
			const std::string &getFirstName() const;
			void setLastName(const std::string &lastName);
			const std::string &getLastName() const;
			//设置雇员的编号
			void setEmployeeNumber(int employeeNumber);
			int  getEmployeeNuber() const;

			//更新雇员的工资
			void setSalary(int newSalary);
			int getSalary() const;
			//雇佣一个员工

			void setHired(bool hire);
			void getIsHired() const;

		private:
			std::string mFirstName;
			std::string mLastName;
			int mEployeeNumber;
			int mSalary;
			bool mHired;
	};
}
