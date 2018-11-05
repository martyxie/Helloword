/**
 * @file chanellogo.c
 * @brief 
 * @author YOUR NAME (), 
 * @version 1.0
 * @history
 * 		参见 :
 * 		2012-11-21 YOUR NAME created
 */

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>

static char *m_log = NULL;
#define BUF_SIZE 1024*5
#define LOGO_LOG( format, ... )  log_out(__func__, __LINE__, format, ##__VA_ARGS__  )

#define CHANNELLOGO_PATH "channelLogo"
#define PRIV_KEY "./lib/priv_key.txt"
#define TOOL_LIB "./lib/swpkgchannelLogo"
#define MAX_LOGO_SIZE 1024*1024*5 //5M
#define OUTPUT_DIR "EC2108CV5@EPG"
#define TMP_MINIEPG_PATH "miniEPG/DVB/customize/res/img/"
#define TMP_CHANELLOGO "miniEPG.tar"
#define BASE_TMP "miniEPG"
#define PACK_CMD "tar -cjf"
#define BOX_NAME "EC2108CV5"
#define HW_VER 685740

static void log_out( const char *fuc,int line,const char* format, ... )
{
#ifdef DEBUG
	va_list args;
	if(m_log == NULL)
		m_log = (char*)malloc(BUF_SIZE);
	if(m_log == NULL)
		return;
	memset(m_log,0,BUF_SIZE);
	//填充日志
	va_start( args, format );
	vsnprintf(m_log,BUF_SIZE,format,args);
	va_end( args );
	fprintf(stdout,"[%s,%d]%s",fuc,line,m_log);
#endif
	return;
	
}

static void show_menue(void)
{
	fprintf(stdout,"\n");
	fprintf(stdout,"***************************************************************\n");
	fprintf(stdout,"** ERROR: Argument is illegal,please check and try again.... **\n");
	fprintf(stdout,"** ./make_logo <DATE> <VERSION>                              **\n");
	fprintf(stdout,"** For example: ./make_channelLogo.sh 20170101 865201        **\n");
	fprintf(stdout,"***************************************************************\n");
	fprintf(stdout,"\n");
	return;
}

//保证都是整数
static bool check_number(char *buf,int size)
{
	if(size == 0 || buf[0]=='\0')
	{
		LOGO_LOG("data illegal\n");
		return false;
	}
	int i = 0;
	for(i=0;i<size;i++)
	{
		if(buf[i] < '0' || buf[i] > '9')
			return false;
	}
	return true;
}

static bool compare_md5(char *path,char *md5_src)
{
	/*
	MD5_CTX ctx;
	struct stat file;
	FILE *fp = NULL;
	int read_size = 0;
	char md5_value[17] = {0};
	char md5_end[34] = {0};
	if(lstat(path,&file) != 0)
	{
		LOGO_LOG("can't find the file %s\n",path);
		return false;
	}
	char *buf = (char *)malloc(file.st_size+8);
	if(buf == NULL)
	{
		LOGO_LOG("faile malloc mem\n");
		return false;
	}
	LOGO_LOG("file.st_size is %d\n",file.st_size);
	memset(buf,0,file.st_size+8);
	fp = fopen(path,"r");
	if(fp == NULL)
	{
		LOGO_LOG("fail to open %s\n",path);
		return -1;
	}

	int tmp_size = 0;
	int ret = 0;
	//读文件
	while(1)
	{
		tmp_size = file.st_size - read_size;
		if(tmp_size <= 0)
		{
			LOGO_LOG("finish to read file\n");
			break;
		}
		if(tmp_size < 102400)
			tmp_size = 102400;
		ret = fread(&buf[read_size],tmp_size,sizeof(char),fp);
		if(ret <= 0)
		{
			LOGO_LOG("finish to read file\n");
			break;
		}
		read_size += ret;
	}
	//计算MD5
	MD5Init(&ctx);
	MD5Update(&ctx,buf,read_size);
	MD5Final(&ctx,md5_value);
	tmp_size = 0;
	for(ret = 0;ret<16;ret++)
		tmp_size += snprintf(&md5_end[tmp_size], sizeof(md5_end), "%02x", md5_value[ret]);
	LOGO_LOG("file %s,md5 is %s\n",md5_end);
	*/
	return true;
}
static int check_and_creat_dir(char *dir)
{
	struct stat file;
	bool need_mkdir = false;
	if(lstat(dir,&file) == 0)
	{
		if(!(S_ISDIR(file.st_mode)))
		{
			if(remove(dir)<0)
			{
				LOGO_LOG("fail to remove file %s\n",dir);
				return -1;
			}
			need_mkdir = true;
		}
	}
	else
		need_mkdir = true;
	if(need_mkdir)
	{
		if(mkdir(dir,0755)<0)
			fprintf(stdout,"\n**fail to created dir %s\n",dir);
		return -1;
	}
	else
		return 0;
}

static bool write_data_to_file(char *file,char *data,int size)
{
	FILE *fp = NULL;
	int wr_size = 0;
	int ret = 0;
	int tmp_size = 0;
	bool result = true;
	LOGO_LOG("file is %s,size = %d\n",file,size);
	fp=fopen(file,"w+");
	if(fp < 0)
	{
		LOGO_LOG("fail to open file %s\n",file);
		return false;
	}
	while(1)
	{
		tmp_size = size - wr_size;
		if(tmp_size > 1024*128)
			tmp_size = 1024*128;

		ret = fwrite(data + wr_size,sizeof(char),tmp_size,fp);
		if(ret <= 0)
			break;
		wr_size += ret;
		LOGO_LOG("wr_size = %d,ret = %d\n",wr_size,ret);
		if(wr_size >= size)
			break;
	}
	if(wr_size != size)
	{
		LOGO_LOG("fail to write file %s,size = %d,wr_size = %d\n",file,size,wr_size);
		result = false;
	}
	fclose(fp);
	return result;
}

static bool read_file_to_data(char *file,char* data,int size)
{
	FILE *fp = NULL;
	int read_size = 0;
	int ret = 0;
	int tmp_size = 0;
	bool result = true;
	struct stat tmp_file;
	if(lstat(file,&tmp_file)!=0)
	{
		LOGO_LOG("file %s not exist\n",file);
		return  false;
	}
	if(size < tmp_file.st_size +1)
	{
		LOGO_LOG("mem is too less\n");
		return false;
	}
	LOGO_LOG("file is %s,size = %d\n",file,size);
	fp=fopen(file,"r");
	if(fp < 0)
	{
		LOGO_LOG("fail to open file %s\n",file);
		return false;
	}
	while(1)
	{
		tmp_size = tmp_file.st_size - read_size;
		if(tmp_size > 1024*128)
			tmp_size = 1024*128;

		ret = fread(data + read_size,sizeof(char),tmp_size,fp);
		if(ret <= 0)
			break;
		read_size += ret;
		LOGO_LOG("read_size = %d,ret = %d\n",read_size,ret);
		if(read_size >= size)
			break;
	}
	if(read_size != size)
	{
		LOGO_LOG("fail to write file %s,size = %d,wr_size = %d\n",file,size,read_size);
		result = false;
	}
	fclose(fp);
	return result;
}

static int prepara_check(void)
{
	struct stat file;
	int count = 0;
	//检查工具是否合法
	if(lstat(TOOL_LIB,&file)==0)
	{
		compare_md5(TOOL_LIB,"0000");

	}
	else
	{
		fprintf(stdout,"\n**fail to load swpkgchannelLogo\n");
		return -1;
	}

	memset(&file,0,sizeof(file));
	//channelLogo 目录是否存在
	if(check_and_creat_dir(CHANNELLOGO_PATH)<0)
	{
		fprintf(stdout,"\n******************************************************\n");
		fprintf(stdout,"****Please copy all logo picture to channelLogo dir **\n");
		fprintf(stdout,"******************************************************\n\n");	
		return -1;
	}
	else //检查文件中的文件以及大小
	{
		DIR *dirp = NULL;
		struct dirent dirbuf;
		struct dirent *presult = NULL; 
		int ret = 0;
		int toltal_size = 0;
		bool need_stop = false;
		char file_name[1024] = {0};
		int fd = 0;
		int sigle_max_size = 1024*15;
		int error_count = 0;
		dirp = opendir(CHANNELLOGO_PATH);
		if(dirp == NULL)
		{
			LOGO_LOG("fail to open dir %s\n",CHANNELLOGO_PATH);
			return -1;
		}
		while(1)
		{
			ret = readdir_r(dirp,&dirbuf,&presult);
			if(ret = 0 || presult == NULL)
			{
				LOGO_LOG("finish to read dir file\n");
				break;
			}
			if(dirbuf.d_name[0] == '.')
			{
				LOGO_LOG("not need to check %s\n",dirbuf.d_name);
				continue;
			}
			memset(&file,0,sizeof(file));
			//filename_ctrl(dirbuf.d_name,file_name,sizeof(file_name));
			memset(file_name,0,sizeof(file_name));
			snprintf(file_name,sizeof(file_name),"%s/%s",CHANNELLOGO_PATH,dirbuf.d_name);
			//LOGO_LOG("file_name = %s\n",file_name);
			if(lstat(file_name,&file) != 0)
			{
				LOGO_LOG("file %s not found \n",dirbuf.d_name);
				continue;
			}
			if(file.st_size > sigle_max_size) //单个文件大于与15K报错
			{
				if(error_count == 0)
					fprintf(stdout,"\n[FATAL] Follows pictures are more than %d Bytes,please remove or replace it\n",sigle_max_size);
				error_count ++;
				fprintf(stdout,"[%03d] \"%s\" size is %d Bytes\n",error_count,dirbuf.d_name,(int)file.st_size);
				need_stop = true;
			}
			count++;
			toltal_size += (file.st_blocks*512);

		}
		//检查单个文件后，对整体包进行一个检查以防漏检
		char *tmp_file = "logo_tmp";
		char cmd[64] = {0};
		snprintf(cmd,sizeof(cmd),"tar -cf %s %s",tmp_file,CHANNELLOGO_PATH);
		if(lstat(tmp_file,&file) == 0)
			remove(tmp_file);
		if(system(cmd))
		{
			LOGO_LOG("fail to pack\n");
			need_stop = true;
		}
		memset(&file,0,sizeof(file));
		if(lstat(tmp_file,&file) == 0)
			remove(tmp_file);
		if(toltal_size > MAX_LOGO_SIZE || file.st_size > MAX_LOGO_SIZE)
		{
			fprintf(stdout,"\n******************************************************\n");
			fprintf(stdout,"*** ERROR,all the logo is more than %d Bytes  ***\n",MAX_LOGO_SIZE);
			fprintf(stdout,"*** Now,all the logo is %7d Bytes            ***\n",toltal_size);
			fprintf(stdout,"*** Please let channelLogo less than %d Bytes ***\n",MAX_LOGO_SIZE);
			fprintf(stdout,"******************************************************\n\n");
			need_stop = true;
		}
		LOGO_LOG("toltal_size = %d,tar size = %d,count = %d\n",toltal_size,file.st_size,count);

		if(need_stop)
			return -1;
		if(count < 1)
		{
			fprintf(stdout,"\n******************************************************\n");
			fprintf(stdout,"****Please copy all logo picture to channelLogo dir **\n");
			fprintf(stdout,"******************************************************\n\n");	
			return -1;
		}
	}
	return 0;

}

static void success_show(void)
{
	fprintf(stdout,"\n*****************************************************\n");
	fprintf(stdout,"**** make channelLogo img success     ***************\n");
	fprintf(stdout,"**** Please copy the file form path %s ***\n",OUTPUT_DIR);
	fprintf(stdout,"*****************************************************\n\n");
}

static void ui_show(int error_code)
{
	switch(error_code) //默认用10100
	{
		case 10100:
			show_menue();
			break;
		case 10101:
			break;
		default:
			break;
	}
	return;
}

static int make_config_file(const char* date,int ver)
{
	int buf_size = 1024;
	char buf[1024] = {0};
	FILE *fp = NULL;
	char file_name[64]={0};
	struct stat file;
	int ret = 0;
	snprintf(file_name,sizeof(file_name),"%s.%s.%d.bin",BOX_NAME,date,ver);
	if(lstat(file_name,&file) != 0)
	{
		LOGO_LOG("fail to get file %s size\n",file_name);
		return -1;
	}
	memset(buf,buf_size,0);
	ret += snprintf(buf,buf_size,"[FIRMWARE]\r\n;Index=1\r\n;AccessVer=[-]\r\n");
	ret += snprintf(buf+ret,buf_size -ret,";Ver=%d\r\n;HWVer=%d\r\n;Size=%d\r\n;File=%s\r\n\r\n",ver,HW_VER,(int)file.st_size,file_name);
	ret +=snprintf(buf+ret,buf_size -ret,"[UPDATEEPGTMPLATE]\r\nVersion=%d\r\nFileName=%s\r\n\r\n",ver,file_name);
	LOGO_LOG("buf == %d\n%s",ret,buf);
	if(!write_data_to_file("config.ini",buf,ret))
	{
		LOGO_LOG("fail to save config.ini\n");
		return -1;
	}
	return 0;
}

static void make_channellogo(char *date,int version)
{
	//检查目录
	char cmd[256] = {0};
	char type[] = "epg";
	if(check_and_creat_dir(OUTPUT_DIR) < 0)
		LOGO_LOG("make new dir %s\n",OUTPUT_DIR);
	else
	{
		snprintf(cmd,sizeof(cmd),"rm -rf %s/*",OUTPUT_DIR);
		system(cmd);
	}
	memset(cmd,sizeof(cmd),0);
	snprintf(cmd,sizeof(cmd),"mkdir %s -p",TMP_MINIEPG_PATH);
	LOGO_LOG("cmd = %s\n",cmd);
	system(cmd);
	memset(cmd,sizeof(cmd),0);
	snprintf(cmd,sizeof(cmd),"cp %s %s -rf",CHANNELLOGO_PATH,TMP_MINIEPG_PATH);
	LOGO_LOG("cmd = %s\n",cmd);
	system(cmd);
	memset(cmd,sizeof(cmd),0);
	snprintf(cmd,sizeof(cmd),"%s %s %s",PACK_CMD,TMP_CHANELLOGO,BASE_TMP);
	LOGO_LOG("cmd = %s\n",cmd);
	system(cmd);
	memset(cmd,sizeof(cmd),0);
	snprintf(cmd,sizeof(cmd),"%s -t %s -v %d -h %d -f %s -o %s.%s.%d.bin > tmp.txt",TOOL_LIB,type,version,HW_VER,TMP_CHANELLOGO,BOX_NAME,date,version);
	LOGO_LOG("cmd = %s\n",cmd);
	system(cmd);
#ifndef DEBUG
	if(access("tmp.txt",F_OK) == 0)
		remove("tmp.txt");
#endif
	make_config_file(date,version);
	success_show();
	return;
}


int main(int argc, char *argv[])
{
	LOGO_LOG("argc = %d\n",argc);
	char date_buf[64] = {0};
	char version[12] = {0};
	int ver = 0;
	int error_code = 10100;
	if(argc != 3)
	{
		show_menue();
		return -1;
	}
	if(argv[1])
		memcpy(date_buf,argv[1],sizeof(date_buf));
	if(argv[2])
		memcpy(version,argv[2],sizeof(version));
	//校验参数
	LOGO_LOG("data_buf = %s,%d,version = %s,%d\n",date_buf,atoi(date_buf),version,atoi(version));
	if((!check_number(date_buf,strlen(date_buf)))||!check_number(version,strlen(version)))
	{
		fprintf(stdout,"\n**date or version is illegal\n");
		sleep(1);
		show_menue();
		return -1;
	}
	ver = atoi(version);
	if(ver < 865200 || ver > 1000000)
	{
		fprintf(stdout,"\n**version number must be Between 865200 and 999999\n");
		show_menue();
		return -1;
	}
	//准备开始
	if(prepara_check()<0)
		;//return -1;
	//制作升级包
	make_channellogo(date_buf,ver);
EXIT:
	ui_show(error_code);
	return 0;
}
