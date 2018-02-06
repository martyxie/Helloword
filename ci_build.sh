#!/usr/bin/env bash 


#shell 脚本中的函数定义关键字为 function name{},或者以 functionname(){}的形式来定义

function my_test_hello
{
	echo "hello ,this is my test"
	return 0
}

function the_second
{
	my_test_hello
	return 0
}

function test_calculation
{
	a=5
	b=6
	c=$[a+b]
	let d=a+b
	e=$((a%b))
	let f=`(expr $a+1)`
	echo $c $d $e $f
	echo "cs = $1"
	return 0

}
#介绍了常用的3中for循环
test_for()
{
	for((i=0;i<10;i++))
	do
		printf "%d " $i;
		if [ $i -eq 9 ] ;
		then
			printf "\n"
		fi
	done
	for i in $(seq 0 9)
	do
		printf "%d " $i;
		if [ $i -eq 9 ] ;
		then
			printf "\n"
		fi
	done
	for i in {0..9}
	do
		printf "%d " $i;
		if [ $i -eq 9 ] ;
		then
			printf "\n"
		fi
	done

	return 0
}


test_while()
{
	i=0
	while [ $i -le 9 ]
	do
		printf "%d " $i;
		if [ $i -eq 9 ] ;
		then
			printf "\n"
		fi
		let i=$i+1
	done
	return 0
}

function test_array()
{
	for((i=0;i<10;i++))
	do
		let array[$i]=$i+1
	done
	for((i=0;i<10;i++))
	do
		printf "%d " $((array[$i]))
		if [ $i -eq 9 ] ;
		then
			printf "\n"
		fi
	done
	printf "%d " ${array[*]}
	printf "\n"
	printf "%s " ${array[*]}
	printf "\n"
	return 0

}

function test_awk()
{
	ls -l

}


#mian
function main()
{
	echo "beigin to test"
	the_second
	test_calculation 22
	test_for
	test_while
	test_array
	test_awk
	return 0;
}
#真正的main函数入口
main
exit 1

