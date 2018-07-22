/*
@file: hwlog_new.c
@brief:默认搜集ERROR日志,保存到flash,同时如果网管下发日志搜集,
       则自动切换到网管下发的日志级别,并将收集到的日志以文件的形式上传
@整体设计思路:
  1.利用swsyslog 模块实现日志的搜集,在本模块实现日志的启动,切换,打包,以及上传
  2.日志收集模块可以认为是一个线程,网管下发参数是一个线程,本模块是一个线程

*/


#include "swapi.h"
#include "swlog.h"
#include "swurl.h"
#include "swparameter.h"
#include "hwlog.h"
#include "hwiptv.h"
#include "hwiptv_priv.h"
#include "swthrd.h"
#include <dirent.h>
#include "hwtime.h"
#include "hwiptv_priv.h"
#include "hwlog_new.h"
#include "hwview.h"
#include "hwtime.h"
#include "hwdevcert.h"
#include "swbrowser.h"

#ifdef SUPPORT_HTTPS
#include "swhttpsclient.h"
#else
#include "swhttpclient.h"
#endif

#define STB_LOG_PATH "/var/log"                  //存放已收集的日志
#define TMP_LOG_BASE_PATH  "/tmp/log"           //临时日志的log
#define TMP_LOG_PATH      "/tmp/log/logtmp"    //日志临时目录
#define MAX_LOG_NUM   5                   //最多保存5个log

#define ERROR_LOG  1 //error日志
#define DEBUG_LOG  2 //debug日志

extern int swsyscmd(const char* cmd );
extern bool sw_get_dvb_sync_time_state(void); //DVB同步时间

typedef struct _level_info
{
	int level;
	unsigned int time_out; //秒
	unsigned int start_time; //秒
}level_info_t;

typedef enum
{
	TMS_LOG_OFF = 0x0,
	TMS_LOG_ERROR,
	TMS_LOG_INFO,
	TMS_LOG_DEBUG,
}tms_log_level_t;

typedef enum
{
	LOG_MODE_NULL,
	LOG_MODE_ERROR,
	LOG_MODE_DEBUG,
	LOG_MODE_USB,
}log_mode_t;

static int m_start_time = 0; //开始抓日志的时间
static int m_end_time = 0; //结束抓日志的时间
static int m_log_level = LOG_LEVEL_OFF; //抓日志的级别
static char m_mac[24];
static char m_stb_type[16];

static int m_default_level = LOG_LEVEL_ERROR;

static int m_log_mode = LOG_MODE_NULL; //当前线程的状态 

HANDLE m_tmslogrun_thrd = NULL; //日志收集,日志检测,日志上传线程

static level_info_t m_chang_info;
static HANDLE m_colloectlog_thrd = NULL;
static bool m_proc_exit = false;

static int m_commit_time = 0; //最后一次打包日志的时间

static sw_url_t m_server_url;              //服务器server
static int m_time_out = 12000;             //超时时间

static int m_interval_debug = 10*60; //默认10分钟,单位是秒
static int m_interval_error = 2*60*60*1000; //默认2小时
static int m_max_size = 3*1024*1024; //3M

static bool m_usb_is_collect = false; //U盘抓日志的标志

//=========文件上传=========

//链接上传日志服务器,双向认证在sw_httpsclient_connect_verify接口中实现
static HANDLE connect_to_logserver(void)
{
	HANDLE client = NULL;
	char log_server[64] = {0};
	int connect_count = 0;
	int time_cnt = 1;
	hw_newlog_tms_get_para("LogServer",log_server,sizeof(log_server));
	HWIPTV_LOG_DEBUG("log_server = %s\n",log_server);
	if(log_server[0] == '\0')
		return NULL;
	char *server_type = NULL;
	void *key = NULL;
#ifdef SUPPORT_HTTPS
	server_type = "https://";
#else
	server_type = "http://";
#endif
	if(strncmp(log_server,server_type,strlen(server_type) != 0))
	{
		HWIPTV_LOG_ERROR("log_server is %s\n",log_server);
		return NULL ;
	}
	sw_memset(&m_server_url,sizeof(m_server_url),0,sizeof(m_server_url));
	if(sw_url_parse(&m_server_url, log_server) != 0)
	{
		HWIPTV_LOG_ERROR("fail to parse log_server is %s\n",log_server);
		return NULL;
	}
	if( m_server_url.ip == 0 || m_server_url.port == 0 )
	{
		HWIPTV_LOG_ERROR("m_server_url.ip == %u, m_server_url.port == %d\n",m_server_url.ip,m_server_url.port);
		return NULL;
	}

	for(connect_count = 0;connect_count < 3;connect_count++) //最大尝试3次链接
	{
#ifdef SUPPORT_HTTPS
#ifdef SUPPORT_MKDEVCERT_ONLINE
		key = sw_dev_private_key_get();
#else
		key = NULL;
#endif
		client = sw_httpsclient_connect_verify( m_server_url.ip, m_server_url.port, m_time_out,key );
#else
		client = sw_httpclient_connect(m_server_url.ip, m_server_url.port, m_time_out);
#endif
		if(client != NULL)
			break;
		else
		{ //参照网管认证经验，如果失败尝试3次链接时间间隔 0,5,10
			sw_thrd_delay(time_cnt*5*1000);
			time_cnt ++;
		}
	}
	return client;
}

//发送日子文件到服务器
static bool send_file_to_server(HANDLE *client,char *file_name,char *s_file_name)
{
	if(client == NULL || file_name == NULL)
		return false;
	if(file_name[0] == '\0')
		return false;
	//POST
	bool ret = false;
	int ret_size = 0;
	struct stat file_stat;
	sw_memset(&file_stat,sizeof(file_stat),0,sizeof(file_stat));
	if(lstat(file_name,&file_stat) != 0)
		return false;
	int pbuf_size = (file_stat.st_size/8 + 1)*8; //8字节对齐
	char *pbuf = (char*) malloc(pbuf_size);
	if(pbuf == NULL)
	{
		HWIPTV_LOG_ERROR("fail to malloc\n");
		return false;
	}
	sw_memset(pbuf,pbuf_size,0,pbuf_size);
	//read log
	FILE *file_fp = fopen(file_name,"r");
	if(file_fp == NULL)
	{
		HWIPTV_LOG_ERROR("fail to open file %s\n",file_name);
		goto ERROR;
	}
	ret_size = fread(pbuf,sizeof(char),file_stat.st_size,file_fp);
	if(ret_size != file_stat.st_size)
	{
		HWIPTV_LOG_ERROR("fail to read file %s,ret_size = %d\n",file_name,ret_size);
		goto ERROR;
	}
#ifdef SUPPORT_HTTPS
	ret = sw_httpsclient_request_ex( client, "POST", m_server_url.path,m_server_url.hostname, NULL, s_file_name,"file/gz", pbuf, ret_size, m_time_out, "", NULL );
#else
	ret = sw_httpclient_request_ex2( client, "POST", m_server_url.path,m_server_url.hostname, NULL, s_file_name,"file/gz", pbuf, ret_size, m_time_out, "", NULL );
#endif
	if(ret == false)
	{
		HWIPTV_LOG_ERROR("fail to POST file %s ,remote patch = %s,hostname = %s\n",file_name,m_server_url.path,m_server_url.hostname);
		goto ERROR;
	}
	//发送成功后清除缓存
	sw_memset(pbuf,pbuf_size,0,pbuf_size);
	//recv
#ifdef SUPPORT_HTTPS
	ret_size = sw_httpsclient_recv_data( client, pbuf,pbuf_size,m_time_out);
#else
	ret_size = sw_httpclient_recv_data( client, pbuf,pbuf_size,m_time_out);
#endif
	HWIPTV_LOG_DEBUG("recv_buf = %s\n",pbuf);
	if(strstr( pbuf, "HTTP/1.1 200" ) != NULL) //服务器接收完毕
		ret = true;
	else
		ret = false;
ERROR:
	if(pbuf)
		free(pbuf);
	if(file_fp)
		fclose(file_fp);
	pbuf = NULL;
	file_fp = NULL;
	return ret;
}

static void disconnect_from_server(HANDLE *client)
{
	if(client == NULL)
		return;
#ifdef SUPPORT_HTTPS
	sw_httpsclient_disconnect(client);
#else
	sw_httpclient_disconnect(client);
#endif
	return;
}

static void get_server_file_name(char *file_name,int size,int type)
{
	struct tm tm;
	time_t now = 0;
	char tmp_time[16] = {0};
	char stb_id[24] = {0};
	if(!sw_parameter_get("serial",stb_id,sizeof(stb_id)))
	{
		HWIPTV_LOG_ERROR("fail to get STB ID \n");
	}
	sw_memset(&tm,sizeof(tm),0,sizeof(tm));
	now = time(NULL);
	localtime_r(&now, &tm);
	strftime(tmp_time,sizeof(tmp_time),"%Y%m%d%H%M%S", &tm);
	if(type == ERROR_LOG)
		sw_snprintf(file_name,size,0,size,"%s_%s_error.gz",stb_id,tmp_time);
	else
		sw_snprintf(file_name,size,0,size,"%s_%s.gz",stb_id,tmp_time);
	return;
}

//日志上传接口，依赖于外部实现，返回成功上传文件的个数
static bool log_file_upload(char *filename)
{
	//先确认是否有文件
	//需要将/var/log/中的日志全部上传
	bool send_ret = true;
	int ret = 0;
	char s_file_name[64] = {0}; //服务器中保存的名字
	char full_name[64] = {0};
	DIR *dirp = NULL;
	struct dirent dirbuf;
	struct dirent *presult = NULL;
	int name_len = 0;
	HANDLE client = NULL;
	int type = 0;

	unsigned int tmp_time = 0;

	sw_snprintf(s_file_name,sizeof(s_file_name),0,sizeof(s_file_name),"errorlog_%s_MMddHHmmss_FFFFFFFFFFFF.tgz",m_stb_type);
	name_len = strlen(s_file_name); //所有日志长度命名都一样长

	dirp = opendir(STB_LOG_PATH);
	if(dirp == NULL)
	{
		HWIPTV_LOG_ERROR("fail to open log dir\n");
		goto TMP_FILE;
	}
	//先检查falsh中是否有要上传的文件
	for( ; ; )
	{
		ret = readdir_r(dirp,&dirbuf,&presult);
		if(ret != 0 || presult == NULL)
		{
			HWIPTV_LOG_INFO("finish to read dir or read error ret = %d,presult=%p\n",ret,presult);
			break;
		}
		HWIPTV_LOG_DEBUG("dirbuf.d_name = %s,name_len = %d\n",dirbuf.d_name,name_len);
		//过滤flash中的目录.
		if(strlen(dirbuf.d_name) == name_len)
		{
			if(strncmp(dirbuf.d_name,"debuglog",strlen("debuglog")) ==0 )
				type = DEBUG_LOG;
			else if(strncmp(dirbuf.d_name,"errorlog",strlen("errorlog")) ==0 )
				type = ERROR_LOG;
			else //如果都不是继续
				continue;
		}
		else
			continue;

		sw_snprintf(full_name,sizeof(full_name),0,sizeof(full_name),"%s/%s",STB_LOG_PATH,dirbuf.d_name);
		// 链接服务器
		if(client == NULL)
			client = connect_to_logserver(); //上传之前进行connect,这里实现双向认证
		if(client) //上传
		{
			//组装服务器端要保存的名字
			sw_memset(s_file_name,sizeof(s_file_name),0,sizeof(s_file_name));
			get_server_file_name(s_file_name,sizeof(s_file_name),type);
			HWIPTV_LOG_DEBUG("full_name = %s,s_file_name = %s\n",full_name,s_file_name);
			tmp_time = sw_thrd_get_tick();
			send_ret = send_file_to_server(client,full_name,s_file_name);
			if(send_ret) //删除本地已上传的文件
				remove(full_name);
			else
			{
				HWIPTV_LOG_ERROR("fail to send log file\n");
				send_ret = false;
				break;
			}
		}
		else
		{
			HWIPTV_LOG_ERROR("fail to conect log server\n");
			send_ret = false;
			break;
		}
		tmp_time = sw_thrd_get_tick() - tmp_time;
		if(tmp_time < 1000) //防止上传太快,在服务器上被覆盖
			sw_thrd_delay(1000 - tmp_time);
		sw_memset(full_name,sizeof(full_name),0,sizeof(full_name));
	}
	closedir(dirp);
TMP_FILE:
	//上传存放在/tmp目录中的debug日志
	if( send_ret && filename[0] != '\0' && client == NULL)
	{
		client = connect_to_logserver(); //上传之前进行connect,这里实现双向认证
		if(client)
		{
			sw_memset(full_name,sizeof(full_name),0,sizeof(full_name));
			sw_memset(s_file_name,sizeof(s_file_name),0,sizeof(s_file_name));
			get_server_file_name(s_file_name,sizeof(s_file_name),DEBUG_LOG); 
			sw_snprintf(full_name,sizeof(full_name),0,sizeof(full_name),"%s/%s",TMP_LOG_BASE_PATH,filename);
			HWIPTV_LOG_DEBUG("full_name = %s,s_file_name = %s\n",full_name,s_file_name);
			send_ret = send_file_to_server(client,full_name,s_file_name);
			if(send_ret) //上传成功,删除本地文件
				remove(full_name);
		}
		else
			send_ret = false;
	}
	if(client)
		disconnect_from_server(client); //上传结束断开链接
	client = NULL;
	return send_ret;
}

//========================公共处理======
//同时开启海思日志
static int set_loglevel(int type)
{
	char cmd[40] = {0};
	int hisi_level = 1; //1 ERROR,2.WARA 3.INFO
	if(type == ERROR_LOG)
	{
		if(m_chang_info.level == LOG_LEVEL_OFF)
			sw_log_set_level(m_default_level);
		hw_view_show_debug_status(false);	
		sw_snprintf(cmd, sizeof(cmd), 0, sizeof(cmd), "echo hi_avplay=1 > /proc/msp/log");
		swsyscmd(cmd);
	}
	else if(type == DEBUG_LOG)
	{
		if(m_chang_info.level == LOG_LEVEL_OFF)
			sw_log_set_level(m_log_level);
		if(m_log_level == LOG_LEVEL_ALL)
		{
			sw_browser_debug(true);
			hisi_level = 1;
		}
		else if(m_log_level == LOG_LEVEL_INFO)
			hisi_level = 2;
		else if(m_log_level == LOG_LEVEL_ERROR) //由于海思日志量太大
			hisi_level = 3;

		if(m_log_level == LOG_LEVEL_ALL)
			hw_view_show_debug_status(true);
		else
			hw_view_show_debug_status(false);

		sw_snprintf(cmd, sizeof(cmd), 0, sizeof(cmd), "echo hi_avplay=%d > /proc/msp/log",hisi_level);
		swsyscmd(cmd);
	}
	HWIPTV_LOG_DEBUG("set_log_level %d,hisi log level %d\n",sw_log_get_level(),hisi_level);
	return 0;
}

//检测falsh中是否保存满指定的文件,如果够了X个则删除最早的.
static int check_save_log(char *path, int type, int max_count)
{
	struct stat file;
	if(lstat(path,&file) != 0)
	{
		HWIPTV_LOG_ERROR("%s is not exist\n",path);
		return -1;
	}
	int i = 0;
	char filename[max_count][128];
	//去掉最早的那份日志
	DIR *dirp = NULL;
	struct dirent dirbuf;
	struct dirent *presult = NULL;
	int ret = 0;
	int count = 0;
	int old_index = 0;
	char *info = NULL;
	int name_len = 0;
	for(i=0; i<max_count; i++)
		sw_memset(filename[i],sizeof(filename[i]),0,sizeof(filename[i]));

	name_len = strlen("errorlog_MMddHHmmss_FFFFFFFFFFFF.tgz");
	if(type == ERROR_LOG)
		info = "errorlog";
	else
		info = "debuglog";

	dirp = opendir(path);
	if(dirp == NULL)
	{
		HWIPTV_LOG_ERROR("fail to open log dir\n");
		return -2;
	}
	while(1)
	{
		ret = readdir_r(dirp,&dirbuf,&presult);
		if(ret != 0 || presult == NULL)
		{
			HWIPTV_LOG_DEBUG("finish to read dir or read error ret = %d,presult=%p\n",ret,presult);
			break;
		}
		if(strncmp(dirbuf.d_name,info,strlen(info)) !=0 || strlen(dirbuf.d_name) != name_len ) //跳过不符合要求的文件
			continue;

		if(count < max_count)
			sw_snprintf(filename[count],sizeof(filename[count]),0,sizeof(filename[count]),"%s",dirbuf.d_name);
		if(count < max_count)
			HWIPTV_LOG_DEBUG("filename[%d] = %s\n",count,filename[count]);

		count++;
	}
	closedir(dirp);
	//移除最新的那个文件 error_201807101234.tbz,根据字符串比较取到最旧的那个文件名
	if(count >= max_count)
	{
		for(i=1;i<max_count;i++)
		{
			if(strncasecmp(filename[i],filename[old_index],name_len)<0)
				old_index = i;
		}
		//删除文件
		char cmd[256] = {0};
		sw_snprintf(cmd,sizeof(cmd),0,sizeof(cmd),"%s/%s",path,filename[old_index]);
		HWIPTV_LOG_DEBUG("remove file %s,old_index = %d\n",cmd,old_index);
		remove(cmd);
		count --;
	}
	return count; //返回目录中有多少个文件
}
static void get_time_info(char *file)
{
	struct tm tmp;
	char time_buf[24] = {0};
	char cmd[120] = {0};
	time_t now = time(NULL);
	localtime_r(&now, &tmp);
	strftime(time_buf,sizeof(time_buf),"%Y%m%d%H%M%S", &tmp);
	sw_snprintf(cmd,sizeof(cmd),0,sizeof(cmd),"echo -----collect time: %s------ >> %s/%s",time_buf,TMP_LOG_PATH,file);
	swsyscmd(cmd);
}

static void get_proc_info(void)
{
	char buf[160] = {0};
	char *proc_info[] = {"/proc/msp/adec00 /proc/msp/avplay00 /proc/msp/avplay01 /proc/msp/disp0 /proc/msp/disp1",
		                 "/proc/msp/hdmi0 /proc/msp/hdmi0_sink /proc/msp/sound0 /proc/msp/sync00 /proc/msp/sync01",
						 "/proc/msp/tuner /proc/msp/vpss00 /proc/msp/win0000 /proc/msp/win0100 /proc/media-mem",
						 "/proc/vfmw*"};
	int i = 0;
	get_time_info("StbProcinfo.txt");
	for(i=0;i < sizeof(proc_info)/sizeof(proc_info[0]);i++ )
	{
		sw_memset(buf,sizeof(buf),0,sizeof(buf));
		sw_snprintf(buf,sizeof(buf),0,sizeof(buf),"cat %s >> %s/StbProcinfo.txt",proc_info[i],TMP_LOG_PATH);
		swsyscmd(buf);
		HWIPTV_LOG_DEBUG("%s\n",buf);
	}
	return;
}

static void get_top_info(void)
{
	char buf[64] = {0};
	get_time_info("top.log");
	sw_snprintf(buf,sizeof(buf),0,sizeof(buf),"top -n 1 >> %s/top.log &",TMP_LOG_PATH);
	swsyscmd(buf);
	return;
}

//判断日志是否满足一定大小,STBlog,proc,hisilog
static int get_collectlog_size(int type)
{
	int ret = 0;
	struct stat file;
	char file_name[64] = {0};

	sw_memset(file_name,sizeof(file_name),0,sizeof(file_name));
	sw_memset(&file,sizeof(file),0,sizeof(file));

	if(type == ERROR_LOG) //todo error 日志只需要监控一个日志文件即可
		sw_snprintf(file_name,sizeof(file_name),0,sizeof(file_name),"%s/error0.log",TMP_LOG_PATH);
	else
		sw_snprintf(file_name,sizeof(file_name),0,sizeof(file_name),"%s/debug0.log",TMP_LOG_PATH);

	if(lstat(file_name,&file) == 0)
		ret += file.st_size;

	sw_memset(file_name,sizeof(file_name),0,sizeof(file_name));
	sw_memset(&file,sizeof(file),0,sizeof(file));
	sw_snprintf(file_name,sizeof(file_name),0,sizeof(file_name),"%s/top.log",TMP_LOG_PATH);
	if(lstat(file_name,&file) == 0)
		ret += file.st_size;

	if(type == DEBUG_LOG) //tms debug日志中有3份日志,STB ,hisi,proc
	{
		sw_memset(&file,sizeof(file),0,sizeof(file));
		sw_snprintf(file_name,sizeof(file_name),0,sizeof(file_name),"%s/stb.log",TMP_LOG_PATH);
		if(lstat(file_name,&file) == 0)
		ret += file.st_size;

		sw_memset(file_name,sizeof(file_name),0,sizeof(file_name));
		sw_memset(&file,sizeof(file),0,sizeof(file));
		sw_snprintf(file_name,sizeof(file_name),0,sizeof(file_name),"%s/StbProcinfo.txt",TMP_LOG_PATH);
		if(lstat(file_name,&file) == 0)
			ret += file.st_size;
	}
	return ret;
}

static void clean_log_targets(int type)
{
	char cmd[40] = {0};
	//设置一个不标准的targets,日志就无法继续输出
	sw_log_set_targets("NULL");
	if(type == DEBUG_LOG)
	{
		sw_memset(cmd,sizeof(cmd),0,sizeof(cmd));
		sw_snprintf(cmd, sizeof(cmd), 0, sizeof(cmd), "echo log= > /proc/msp/log");
		swsyscmd(cmd);
	}
	return;
}
static void set_log_targets(int type)
{
	char cmd[128] = {0};
	struct stat path;
	//if(type == ERROR_LOG)
	if(lstat(TMP_LOG_PATH,&path) != 0)
		mkdir(TMP_LOG_PATH,600);
	if(type == ERROR_LOG)
	{
#ifdef SUPPORT_OPEN_STBMONITOR_SERIAL
		sw_snprintf(cmd,sizeof(cmd),0,sizeof(cmd),"file:///%s/error.log&max_size=4M,localsock"/*console*/,TMP_LOG_PATH);
#else
		sw_snprintf(cmd,sizeof(cmd),0,sizeof(cmd),"file:///%s/error.log&max_size=4M",TMP_LOG_PATH);
#endif
	}
	else
	{
#ifdef SUPPORT_OPEN_STBMONITOR_SERIAL
		 sw_snprintf(cmd,sizeof(cmd),0,sizeof(cmd),"file:///%s/debug.log&max_size=4M,localsock"/*,console"*/,TMP_LOG_PATH);
#else
		 sw_snprintf(cmd,sizeof(cmd),0,sizeof(cmd),"file:///%s/debug.log&max_size=4M",TMP_LOG_PATH);
#endif
	}
	sw_log_set_targets(cmd);

	sw_memset(cmd,sizeof(cmd),0,sizeof(cmd));
	if(type == DEBUG_LOG)
	{
		sw_snprintf(cmd, sizeof(cmd), 0, sizeof(cmd), "echo log=%s > /proc/msp/log",TMP_LOG_PATH);
		HWIPTV_LOG_DEBUG("cmd: %s\n",cmd);
		swsyscmd(cmd);
	}
	return;
}
//将临时文夹改成约定格式并输出
static int chang_tmp_log_path(int type,char *log_path,int size)
{
	struct tm tm;
	sw_memset(&tm,sizeof(tm),0,sizeof(tm));
	char file[64] = {0};
	char cmd[128] = {0}; 
	time_t now = time(NULL);
	if(localtime_r(&now, &tm)==NULL)
	{
		HWIPTV_LOG_ERROR("get localtime error\n");
		return -1;
	}
	if(type == ERROR_LOG)
		sw_snprintf(file,sizeof(file),0,sizeof(file),"errorlog_%s_%02d%02d%02d%02d%02d_%s",m_stb_type,tm.tm_mon+1,tm.tm_mday,tm.tm_hour,tm.tm_min,tm.tm_sec,m_mac);
	else //debug
		sw_snprintf(file,sizeof(file),0,sizeof(file),"debuglog_%s_%02d%02d%02d%02d%02d_%s",m_stb_type,tm.tm_mon+1,tm.tm_mday,tm.tm_hour,tm.tm_min,tm.tm_sec,m_mac);

	sw_snprintf(cmd,sizeof(cmd),0,sizeof(cmd),"mv %s %s/%s",TMP_LOG_PATH,TMP_LOG_BASE_PATH,file);
	swsyscmd(cmd); //修改名称
	sw_snprintf(log_path,size,0,size,"%s",file);
	return 0;
}


//将目录strpath 打包后存放在destpath,返回打包后的文件名
static int pack_and_compress_log(int type,char *src_path,char *file_name,int size)
{
	char cmd[128] = {0}; 
	if(type == DEBUG_LOG) //复制参数,频道列表过来
	{
		sw_memset(cmd,sizeof(cmd),0,sizeof(cmd));
		sw_snprintf(cmd,sizeof(cmd),0,sizeof(cmd),"cp /var/dvb/channellist.dat /var/.iptv/para /var/revise.history %s/%s -f",TMP_LOG_BASE_PATH,src_path);
		HWIPTV_LOG_DEBUG("cmd : %s\n",cmd);
		swsyscmd(cmd);
	}
	sw_memset(cmd,sizeof(cmd),0,sizeof(cmd));
	sw_snprintf(cmd,sizeof(cmd),0,sizeof(cmd),"busybox tar -czf %s/%s.tgz -C %s %s",TMP_LOG_BASE_PATH,src_path,TMP_LOG_BASE_PATH,src_path);
	HWIPTV_LOG_DEBUG("cmd : %s\n",cmd);
	swsyscmd(cmd); //打包压缩
	//删除原目录
	sw_memset(cmd,sizeof(cmd),0,sizeof(cmd));
	sw_snprintf(cmd,sizeof(cmd),0,sizeof(cmd),"rm %s/%s -rf",TMP_LOG_BASE_PATH,src_path);
	HWIPTV_LOG_DEBUG("cmd : %s\n",cmd);
	swsyscmd(cmd); //打包压缩
	sw_snprintf(file_name,size,0,size,"%s.tgz",src_path);
	return 1;
}

//将strptah目录下的filename移动到destpath目录中,type说明该文件是什么文件
static void save_file_to_flash(char *filename,int type)
{
	char cmd[256] = {0};
	int ret = 0;
	for(;;) //移动前,先检测目标目录是否已达最大值
	{
		ret = check_save_log(STB_LOG_PATH,type,MAX_LOG_NUM);
		if(ret<MAX_LOG_NUM)
			break;
	}
	//mv /xxxxx/xxxxx.tgz /xxxx
	sw_snprintf(cmd,sizeof(cmd),0,sizeof(cmd),"mv %s/%s %s",TMP_LOG_BASE_PATH,filename,STB_LOG_PATH);
	HWIPTV_LOG_DEBUG("cmd : %s\n",cmd);
	swsyscmd(cmd);
	return;
}

//临时修改日志级别,timeout,是抓该级别的抓多久
void hw_set_tmp_loglevel(int level,int timeout)
{
	if(level > LOG_LEVEL_OFF || level < LOG_LEVEL_ALL)
	{
		HWIPTV_LOG_DEBUG("level illegal\n");
		return;
	}
	m_chang_info.level = level;
	m_chang_info.time_out=timeout;
	m_chang_info.start_time = sw_thrd_get_tick()/1000;
	HWIPTV_LOG_INFO("log_level %d ---> %d\n",sw_log_get_level(),level);
	sw_log_set_level(level);
}


//统一处理日志(上传或保存)
static void commit_log_file(int type,char *filename)
{
	printf("commit file %s,type = %d\n",filename,type);
	int ret = 0;
	char save_name[64] = {0};
	if(filename[0] != '\0')
	{
		ret = pack_and_compress_log(type,filename,save_name,sizeof(save_name));
	}

	if(type == ERROR_LOG && ret > 0)
		save_file_to_flash(save_name,type);	
	else
		if(!log_file_upload(save_name) && ret > 0)
			save_file_to_flash(save_name,type);
	HWIPTV_LOG_DEBUG("finish  to commit log\n");
	return;
}


static int collect_log_check(int type)
{
	int tmp_time = 0;
	int collect_size = 0;
	int max_time = 0;
	if(type == ERROR_LOG)
		max_time = m_interval_error;
	else
		max_time = m_interval_debug;
		
	if(m_chang_info.time_out > 0) //预留功能,切换日志级别一段时间
	{
		tmp_time = (sw_thrd_get_tick()/1000 - m_chang_info.start_time);

		if(tmp_time > m_chang_info.time_out) //恢复日志
		{
			int log_level = 0;
			m_chang_info.time_out = 0;
			m_chang_info.level = LOG_LEVEL_OFF;
			if(type == ERROR_LOG)
				log_level = m_default_level;
			else
				log_level = m_log_level;
			HWIPTV_LOG_INFO("sw_log_set_level %d ---> %d\n",sw_log_get_level(),log_level);
			sw_log_set_level(log_level);
		}
	}
	tmp_time = sw_thrd_get_tick()/1000 - m_commit_time;
	//检测日志文件是否达到最大文件标准,error0.log
	collect_size = get_collectlog_size(type);
	printf("collect_szie = %d K,tmp_time = %d\n s",collect_size/1024,tmp_time);
	if(collect_size >= m_max_size || tmp_time >= max_time) //固定条件处理 
	{
		char file_name[64] = {0};
		HWIPTV_LOG_DEBUG("collect_szie = %dK,tmp_time = %ds\n",collect_size/1024,tmp_time);
		clean_log_targets(type);
		chang_tmp_log_path(type,file_name,sizeof(file_name));
		set_log_targets(type);
		commit_log_file(type,file_name);
		m_commit_time = sw_thrd_get_tick()/1000;
	}
	return 0;

}

//=======TMS log========================================================================
//tms log level 转换成swlog level
static int get_swlog_level(int tmp)
{
	int ret = -1;
	HWIPTV_LOG_DEBUG("tpm = %d\n",tmp);
	switch(tmp)
	{
		case TMS_LOG_DEBUG:
			ret = LOG_LEVEL_ALL;
			break;
		case TMS_LOG_INFO:
			ret = LOG_LEVEL_INFO;
			break;
		case TMS_LOG_ERROR:
			ret = LOG_LEVEL_ERROR;
			break;
		default:
			ret = LOG_LEVEL_OFF;
	}
	return ret;
}


//只读接口,值通过value返回
int hw_newlog_tms_get_para(char *name,char *value,int size)
{
	char tmp_buf[64] = {0};
	if(strncasecmp(name,"LogLevel",strlen("LogLevel"))==0) //日志级别
	{
		if(!sw_parameter_get("loglevel",tmp_buf,sizeof(tmp_buf)))
			sw_snprintf(tmp_buf,sizeof(tmp_buf),0,sizeof(tmp_buf),"0");	
		
	}
	else if(strncasecmp(name,"LogServer",strlen("LogServer"))==0)
	{
		sw_parameter_get("logserver",tmp_buf,sizeof(tmp_buf));
	}
	else if(strncasecmp(name,"LogStartTime",strlen("LogStartTime"))==0)
	{
		if(!sw_parameter_get("logstarttime",tmp_buf,sizeof(tmp_buf)))
			sw_snprintf(tmp_buf,sizeof(tmp_buf),0,sizeof(tmp_buf),"0");
		else
			sw_snprintf(tmp_buf,sizeof(tmp_buf),strlen(tmp_buf),sizeof(tmp_buf),"000"); //变成毫秒
	}
	else if(strncasecmp(name,"LogEndTime",strlen("LogEndTime"))==0) //日志结束收集的时间
	{
		if(!sw_parameter_get("logstarttime",tmp_buf,sizeof(tmp_buf)))
			sw_snprintf(tmp_buf,sizeof(tmp_buf),0,sizeof(tmp_buf),"0");
		else
			sw_snprintf(tmp_buf,sizeof(tmp_buf),strlen(tmp_buf),sizeof(tmp_buf),"000"); //变成毫秒
	}
	else
	{
		HWIPTV_LOG_DEBUG("unknow para\n");
		return -1;
	}
	sw_snprintf(value,size,0,size,"%s",tmp_buf);
	return 0;
}

//参数设置
int hw_newlog_tms_set_para(char *name,char *value,int size)
{
	char tmp_buf[64] = {0};
	HWIPTV_LOG_DEBUG("name = %s,value = %s,size = %d\n",name,value,size);

	if(name == NULL || value == NULL || value[0] == '\0' ||size > sizeof(tmp_buf))
	{
		HWIPTV_LOG_DEBUG("para error name = %s,value = %s,size = %d\n",name,value,size);
		return -1;
	}

	unsigned int tmp = 0;
	sw_memcpy(tmp_buf,sizeof(tmp_buf),value,size,size);
	if(strncasecmp(name,"LogLevel",strlen("LogLevel"))==0) //日志级别
	{
		tmp = atoi(tmp_buf);
		m_log_level = get_swlog_level(tmp);
		sw_parameter_set("loglevel",tmp_buf); //保存TMS的值
		//在行过程下发日志级别的变更
		if(m_log_mode == LOG_MODE_DEBUG && m_log_level < LOG_LEVEL_OFF) 
			set_loglevel(DEBUG_LOG);

	}
	else if(strncasecmp(name,"LogServer",strlen("LogServer"))==0) //日志上传服务器
	{
		sw_parameter_set("logserver",tmp_buf);
	}
	else if(strncasecmp(name,"LogStartTime",strlen("LogStartTime"))==0) //日志开始搜集的时间
	{
		tmp = strlen(tmp_buf);
		//服务器下发的ms,使用时转换成秒(即去掉最后3位)
		tmp_buf[tmp-3] = '\0';
		tmp = atoi(tmp_buf);
		sw_parameter_set_int("logstarttime", tmp);
		m_start_time = tmp;
	}
	else if(strncasecmp(name,"LogEndTime",strlen("LogEndTime"))==0) //日志结束收集的时间
	{
		tmp = strlen(tmp_buf);
		//服务器下发的ms,使用时转换成秒(即去掉最后3位)
		tmp_buf[tmp-3] = '\0';
		tmp = atoi(tmp_buf);
		sw_parameter_set_int("logendtime", tmp);
		m_end_time = tmp;
	}
	else
	{
		HWIPTV_LOG_DEBUG("unknown para %s\n",name);
		return -1;
	}
	sw_parameter_save();
	return 0;
}

//==========================function=======================
static bool collect_log_proc(uint32_t wparam,uint32_t lparam)
{
	time_t now = 0;
	int count = 0;
	bool time_sync = false; //是否已同步时间
	char file_name[64] = {0};
	while(!m_proc_exit)
	{
		if(!time_sync) //检测是否已同步时间
		{
			if(hw_time_get_state() || sw_get_dvb_sync_time_state())
				time_sync = true;
			else
				HWIPTV_LOG_DEBUG("sys time is not sync\n");
		}

		now = time(NULL);
		if(m_usb_is_collect) //优先级最高
		{
			int type = 0;
			if(m_log_mode != LOG_MODE_USB )
			{
				if(m_log_mode == LOG_MODE_ERROR)
					type = ERROR_LOG;
				else if(m_log_mode == LOG_MODE_DEBUG)
					type = DEBUG_LOG;
				if(type)
				{
					sw_memset(file_name,sizeof(file_name),0,sizeof(file_name));
					clean_log_targets(type);
					chang_tmp_log_path(type,file_name,sizeof(file_name));
					commit_log_file(type,file_name); //将flash中所有日志上传
				}
				m_log_mode = LOG_MODE_USB;
			}
		}
		//TMS off,|| time_sync = false || m_start_time < now || now > m_end_time
		else if(m_log_level < LOG_LEVEL_OFF && time_sync && m_start_time < now && now < m_end_time) //TMS_抓日志条件
		{
			//启动搜集日志
			printf("DEBUG =========\n");
			if(m_log_mode != LOG_MODE_DEBUG)
			{
				sw_memset(file_name,sizeof(file_name),0,sizeof(file_name));
				count = 0;
				if(m_log_mode == LOG_MODE_ERROR)
				{
					clean_log_targets(ERROR_LOG);
					if(get_collectlog_size(ERROR_LOG) > 0)
					{
						printf("DEBUG  chagc=========\n");
						chang_tmp_log_path(ERROR_LOG,file_name,sizeof(file_name));
					}
				}
				set_log_targets(DEBUG_LOG);
				set_loglevel(DEBUG_LOG);
				//如果是模式切换,则需要将已收集的日志commit
				if(m_log_mode == LOG_MODE_ERROR)
				{
					printf("commit file %s\n",file_name);
					commit_log_file(ERROR_LOG,file_name); //保存err日志
					sw_memset(file_name,sizeof(file_name),0,sizeof(file_name));
					commit_log_file(DEBUG_LOG,file_name); //将flash中所有日志上传
					m_commit_time = sw_thrd_get_tick()/1000;
				}
				m_log_mode = LOG_MODE_DEBUG;
			}
			if(count == 0 || count >= 60) //1分获取一次
			{
				count = 0;
				printf(" ======get top===\n");
				get_top_info();
				get_proc_info();
			}
			collect_log_check(DEBUG_LOG);
		}
		else //ERROR日志
		{
			//启动日志搜集
			printf("ERROR =========\n");
			if(m_log_mode != LOG_MODE_ERROR)
			{
				count = 0;
				sw_memset(file_name,sizeof(file_name),0,sizeof(file_name));	
				if(m_log_mode == LOG_MODE_DEBUG)
				{
					clean_log_targets(DEBUG_LOG);
					if(get_collectlog_size(DEBUG_LOG) > 0)
					{
						printf("ERROR =chang========\n");
						chang_tmp_log_path(DEBUG_LOG,file_name,sizeof(file_name));
					}
				}
				set_log_targets(ERROR_LOG);
				set_loglevel(ERROR_LOG);
				//如果是模式切换,则需要将已收集的日志commit //这里会上传日志
				if(m_log_mode == LOG_MODE_DEBUG)
				{
					commit_log_file(DEBUG_LOG,file_name);
					m_commit_time = sw_thrd_get_tick()/1000;
				}
				m_log_mode = LOG_MODE_ERROR;
			}
			if(count == 0 || count >= 60) //1分获取一次
			{
				count = 0;
				get_top_info();
			}
			collect_log_check(ERROR_LOG);	
		}
		count ++;
		sw_thrd_delay(1000);
	}
	m_colloectlog_thrd = NULL;
	clean_log_targets(DEBUG_LOG);
	return false;
}

//开始抓日志
void hw_new_log_init(void)
{
	//创建所有目录
	struct stat path;
	char buf[64] = {0};
	int i = 0;
	int j = 0;
	//初始化目录
	if(lstat(TMP_LOG_BASE_PATH,&path) != 0)
		mkdir(TMP_LOG_BASE_PATH,600);
	if(lstat(STB_LOG_PATH,&path) != 0)
		mkdir(STB_LOG_PATH,600);
	//获取盒子的mac地址
	if(!sw_parameter_get("mac",buf,sizeof(buf)))
	{
		sw_snprintf(buf,sizeof(buf),0,sizeof(buf),"FFFFFFFFFFFF");
		HWIPTV_LOG_DEBUG("can't get mac\n");
	}
	else
	{
		sw_memset(m_mac,sizeof(m_mac),0,sizeof(m_mac));
		for(i=0; i< strlen(buf);i++)
		{
			if(buf[i] == ':')
				continue;
			m_mac[j]=buf[i];
			j++;
		}	
	}
	//初始化可变日志信息
	m_chang_info.time_out = 0;
	m_chang_info.level = LOG_LEVEL_OFF;
	//初始化debug日志参数
	sw_memset(buf,sizeof(buf),0,sizeof(buf));
	if(sw_parameter_get("loglevel",buf,sizeof(buf)))
		m_log_level = get_swlog_level(atoi(buf));
	else
		 HWIPTV_LOG_WARN("fail to get the para: loglevel\n");

	sw_memset(buf,sizeof(buf),0,sizeof(buf));
	if(sw_parameter_get("logstarttime",buf,sizeof(buf)))
		m_start_time = atoi(buf);
	else
		 HWIPTV_LOG_WARN("fail to get the para: logstarttime\n");

	sw_memset(buf,sizeof(buf),0,sizeof(buf));
	if(sw_parameter_get("logendtime",buf,sizeof(buf)))
		m_end_time = atoi(buf);
	else
		HWIPTV_LOG_WARN("fail to get the para: logendtime\n");

	sw_memset(m_stb_type,sizeof(m_stb_type),0,sizeof(m_stb_type));
	if(!sw_parameter_get("hardware_type",m_stb_type,sizeof(m_stb_type)))
	{
		sw_snprintf(m_stb_type,sizeof(m_stb_type),0,sizeof(m_stb_type),"EC2108CV5");
		HWIPTV_LOG_WARN("fail to get the para: hardware_type\n");
	}

	//初始化error日志保存时间
	sw_memset(buf,sizeof(buf),0,sizeof(buf));
	if(sw_parameter_get("TimeIntervalForLogToFlash",buf,sizeof(buf)))
	{
		m_interval_error = atoi(buf);
		if(m_interval_error < 10*60) //防止测试设置时间过短
			m_interval_error = 10*60;
	}
	else
		HWIPTV_LOG_WARN("fail to get the para:TimeIntervalForLogToFlash\n");

	//启动检测线程
	m_colloectlog_thrd = sw_thrd_open( "tCollectlog_thrd", 168, 0, 1024*8,collect_log_proc ,0,0);
	if(m_colloectlog_thrd == NULL)
	{
		HWIPTV_LOG_ERROR("fail to open tErrorlog_thrd\n");
		return;
	}
	sw_thrd_resume(m_colloectlog_thrd);
	//删除上个版本的日志,防止占用空间
	return;
}
//压缩信息,停止搜集日志
void hw_new_log_exit(void)
{
	m_proc_exit = true;
	return;
}
int hw_usb_collectlog(bool usb)
{
	int count = 0;
	m_usb_is_collect = usb;
	if(usb)
	{
		for(;;)
		{
			if(m_log_mode == LOG_MODE_USB)
				break;
			if(count > 25)
				break;
			count ++;
			sw_thrd_delay(300);
		}
	}
	else if(m_log_mode == LOG_MODE_USB)
		m_log_mode = LOG_MODE_NULL;
	return 0;
}
