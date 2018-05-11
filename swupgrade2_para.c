/*file:swupgrade_para.c
 *briefs:灭顶之灾 UPG参数升级
 *       将congfig中的信息统一保存到该文件,各局点获取该信息后可进行相应的定制.
 *date:20180502
 *history:noe
 *设计思想:借助原来的升级模块,获取config文件中的[paramreset]标签信息
 *         在应用层,不同的局点获取到信息后可以进行相应的定制
 */
#include "swapi.h"
#include "swupgrade2_priv.h"
#include "swupgrade2_para.h"
//=============
#include "swparameter.h"
#include "swfile.h"
#include "swssl.h"

static swupg_para_t m_upg;

void sw_upgpara_info_reset(void)
{
	sw_memset(&m_upg,sizeof(m_upg), 0, sizeof(m_upg));
	m_upg.is_reboot = 1; //默认强制升级后重启
	return;
}
//设置相关信息到
void upgpara_info_set( swupg_para_t *file,int size)
{
	if(file==NULL || size > sizeof(m_upg))
	{
		sw_log_error("fail to set para info\n");
		return;
	}
	sw_memcpy(&m_upg,sizeof(m_upg),file,size,size);
	return;	
}

//设置服务文件的获取路径
void sw_upgpara_set_readdir(char *dir,int size)
{
	if(dir == NULL || size > sizeof(m_upg.filepath) || m_upg.version[0]=='\0')
	{
		sw_log_error("fail to set file dir\n");
		return;
	}
	sw_memset(m_upg.filepath,sizeof(m_upg.filepath),0,sizeof(m_upg.filepath));
	sw_snprintf(m_upg.filepath,sizeof(m_upg.filepath),0,sizeof(m_upg.filepath),"%s%s",dir,m_upg.filename);
	return;
}
//获取信息
bool sw_upgpara_get_para_info(swupg_para_t *file,int size)
{
	if(file == NULL || size < sizeof(swupg_para_t))
		return false;
	sw_memcpy(file,size,&m_upg,sizeof(m_upg),sizeof(m_upg));
	return true;
}

//================test====

static bool date_is_right(const char *buf)
{
	if(buf == NULL)
		return false;
	int i = 0;
	int len = strlen(buf);
	char tmp[12] = {0};
	if(len != 8+3) //规格规定为 8+3
		return  false;
	sw_memcpy(tmp,sizeof(tmp),buf,len,len);
	for(i=0;i<len;i++) //先检测是否为纯数字
	{
		if(buf[i]< '0' || buf[i] >'9' )
			return false;
	}
	//对日期进行进一步校验,前8字节
	tmp[8] = '\0';
	if(atoi(&tmp[6]) > 31)
		return false;
	else
	{
		tmp[6] = '\0';
		if(atoi(&tmp[4]) > 12)
			return false;
	}
	return true;
}

static bool check_valiable(const char *buf)
{
	//length == 8+3+局点信息
	char hard_type[18] = {0};
	const char *p = NULL;
	if(!sw_parameter_get("hardware_type",hard_type,sizeof(hard_type)))
	{
		printf("fail to get hardware_type\n");
		strlcpy(hard_type,"EC2108CV5",sizeof(hard_type));
	}
	int len = strlen(hard_type)+8+3;
	if(strlen(buf) != len) //检测长度是否正确
	{
		printf("version is not right\n");
		return false;
	}
	//检测版本信息是否正确
	if(strncmp(buf,hard_type,strlen(hard_type)) != 0)
	{
		printf("hardware_type is not match\n");
		return false;
	}
	//检测版本号是否正确
	p=buf+strlen(hard_type);
	if(!date_is_right(p))
	{
		printf("data is not valiable\n");
		return false;
	}
	return true;
}

static bool check_para_config_setting(swupg_para_t *file,int size)
{
	swupg_para_t upg_para;
	if(file == NULL || size > sizeof(upg_para))
		return false;
	sw_memset(&upg_para,sizeof(upg_para),0,sizeof(upg_para));
	sw_memcpy(&upg_para,sizeof(upg_para),file,size,size);
	char *p = NULL;
	int len = 0;
	char last_verison[32] = {0};
	//3个关键信息必须都同时存在
	if(upg_para.version[0] == '\0' || upg_para.filename[0] == '\0' || upg_para.checkcode[0] == '\0')
	{
		printf("version or filename or checkcode is not exist\n");
		return false;
	}
	//检测有有效性
	if(check_valiable(upg_para.version))
	{
		//配置文件的名字
		p = strstr(upg_para.filename,".");
		if(p)
			*p = '\0';
		len = strlen(upg_para.filename);
		if(len == strlen(upg_para.version))
		{
			if(strncmp(upg_para.version,upg_para.filename,len) != 0)
			{
				printf("para filename and version is not macth\n");
				return false;
			}
		}
		else
			return false;
	}
	else
	{
		printf("version number no match\n");
		return false;
	}
	//检测版本号是否更新
	sw_parameter_get("last_para_update_ver",last_verison,sizeof(last_verison));
	if(last_verison[0] != '0')
	{
		if(strncmp(upg_para.version,last_verison,len) <= 0)
		{
			printf("last_para_update_ver is %s\n",last_verison);
			return false;
		}
	}
	return true;
}

typedef struct _para_t
{
	char name[256];
	char value[256];
}para_t;

//升级参数,只升级存在的参数不存在的不升级,权限控制
static int upgrade_para_data(char *buf,int size)
{
	if(buf == NULL || buf[0] == '\0')
		return -1;
	para_t para_upg;
	char tmp_value[256] = {0};
	int strsize = strlen(buf);
	if(strsize > size)
		strsize = size;
	char *p, *e, *name_s, *name_e;
	int len = 0;
	int num = 0;
	//分析数据
	printf("buf = %s\n",buf);
	p = buf;
	while(p < buf + size)
	{
		//去掉开始的空格
		while( *p == ' ' ||  *p == '\t' ||  *p == '\r' ||  *p == '\n' )
			p++;
		name_s = p;
		//找到一行的结束
		while( *p != '\r' && *p != '\n' && *p != '\0' )
			p++;
		e = p;

		/* 忽略: 注释,空行,老版本参数信息 */ 
		if( *name_s == '#' || e <= name_s)
			goto NEXT_ROW;
		/* 找到name/value分隔符，得到参数名称 */
		p = name_s;
		while( *p != ' ' && *p != '\t' && *p != ':' && *p != '=' && p < e )
			p++;
		if( p <= name_s )
			goto NEXT_ROW;
		name_e = p;
		
		//找到 value
		p++;
		while( *p == ' ' || *p == '\t' || *p == ':' || *p == '=' )
			p++;
		if(p <= e)
		{
			//初始化缓存
			sw_memset(&para_upg,sizeof(para_upg),0,sizeof(para_upg));
			sw_memset(tmp_value,sizeof(tmp_value),0,sizeof(tmp_value));
			//将name 保存起来
			len = name_e - name_s;
			sw_memcpy(para_upg.name,sizeof(para_upg.name),name_s,len,len);
			//将值保存起来
			len = e - p;
			sw_memcpy(para_upg.value,sizeof(para_upg.value),p,len,len);
			
			if(para_upg.name[0] == '\0' || para_upg.value[0] == '\0')
				goto NEXT_ROW;

			if(sw_parameter_get(para_upg.name,tmp_value,sizeof(tmp_value))) //确保每次只升级已存在的参数
			{
				sw_parameter_set(para_upg.name,para_upg.value);
				num ++;
			}
		}
NEXT_ROW:
		p = e + 1;
	}
	if(num > 0)
		sw_parameter_save();

	return num;
}

//do updata
static bool upgrade_para(const swupg_para_t *upg)
{
	if(upg == NULL || upg->filepath[0] == '\0')
	{
		printf("no file path\n");
		return false;
	}
	int timeout = 15000;
	int file_size = 0;
	char *data_buf = NULL;
	int read_size = 0; //读取的大小
	int recv_size = 0; //已读的大小
	int ret = 0; //实际读到的大小
	int left_size = 0;  //剩余要读的大小
	unsigned char chip_text[32] = {0};
	char checkcode[68] = {0};// 计算sha256的值
	bool result = false;
	bool need_upg = true;
	int i = 0;
	int pos = 0;
	char *p = NULL;
	char *tmp = NULL;

	swfile_t *fp = sw_file_open_ex(upg->filepath, "r", timeout, "connecttimes=2000,4000,8000\0");//华为升级规格需要
	if(fp == NULL)
	{
		printf("can't to find the  file\n");
		return false;
	}
	file_size = sw_file_get_size(fp);
	if(file_size <= 0)
	{
		printf("not content \n");
		goto ERROR;
	}
	data_buf = (char *)malloc(file_size+8);//预留8字节
	if(data_buf == NULL)
	{
		printf("fail to malloc mem\n");
		goto ERROR;
	}
	sw_memset(data_buf,file_size+8,0,file_size+8);
	
	left_size = file_size; 
	//下载数据
	do{
		recv_size += ret;
		left_size -= ret;
		read_size = left_size > 160*1024 ? 160*1024:left_size;
	}while((ret = sw_file_read( fp,data_buf+recv_size,read_size)) > 0);
	if(recv_size != file_size)
	{
		printf("fail to recv data\n");
		goto ERROR;
	}
	//对比sha256的值
	ret = sw_sha256_sum(chip_text, (unsigned char *)data_buf,file_size);
	for(i=0;i<ret;i++)
		pos += snprintf(&checkcode[pos],sizeof(checkcode)-pos,"%02x",chip_text[i]);
	if(strncmp(checkcode,upg->checkcode,pos) != 0)
	{
		printf("checkcode is no macth\n");
		goto ERROR;
	}
	//备份将要升级的数据

	//找到参数的空间(TODO 目前暂且认证有效参数只存在于 这个两个标签之间)
	p = strstr( data_buf,"[TMWParameterResetList]");
	tmp = strstr( data_buf,"[STBFliterList]");
	if(tmp) //查下是否符合升级条件
	{
		need_upg = true;
	}
	if(need_upg && p)
	{
		*tmp = '\0';
		p += strlen("[TMWParameterResetList]");
		ret = upgrade_para_data(p,tmp - p);
		if(ret > 0)
			printf("%d param had finish reset\n", ret);
		else
			printf("ret is %d\n",ret);
	}

	result = true;
ERROR:
	if(fp)
		sw_file_close(fp);
	if(data_buf)
		free(data_buf);
	return result;
}

void sw_upgpara_begin(void)
{
	if(!check_para_config_setting(&m_upg,sizeof(m_upg)))
	{
		printf("not any para data to do\n");
		return;
	}
	else
	{
		if(!upgrade_para(&m_upg))
		{
			printf("update para faile or not allow to upg para\n");
			return;
		}

		//sw_parameter_set("last_para_update_ver",m_upg.version);
		//sw_parameter_save();
		char tmp[128] = {0};
		if( !sw_parameter_get("hhdjajdheh",tmp,sizeof(tmp)))
		{
			printf("can't to find the para\n");
		}
	}
	if(m_upg.is_reboot == 1)
	{
		sleep(3);
		printf("reboot\n");
	}
}
