#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <pthread.h>
#include <fcntl.h>

//定义一个结构体对线程工作所需数据进行封装
typedef struct Data
{
	int pthreadnum;
	int nNum;
	int blocksize;
	int filesize;
	void *srcptr, *desptr;
}Info;

int block_cut(const char *srcfile, int pthreadnum, Info *info)
{
	int fd, fileSize;
	Info *pInfo = info;

	fd = open(srcfile, O_RDONLY);
	if(fd == -1)
	{
		perror("Open Failed");
		return -1;
	}

	fileSize = lseek(fd, 0, SEEK_END);
	if(fileSize == -1)
	{
		perror("FileSize Failed");
		close(fd);
		return -1;
	}
	pInfo->filesize = fileSize;
	
	close(fd);
	pInfo->blocksize = fileSize/pthreadnum;
}

void *threadjob(void *pInfo)
{
	//经过MMAP映射文件至内存后，应用memcpy
	//该函数是C和C++使用的内存拷贝函数
	Info *info = (Info *)pInfo;
	printf("pthread %d is copying\n", info->nNum);
	int pos = info->nNum * info->blocksize;
	
	//只对最后一次拷贝进行特殊处理，以免拷贝数据不完整
	//使用memcpy函数时，需要注意：
	//数据长度（第三个参数）的单位是字节（1byte = 8bit）。
	if(info->nNum < info->pthreadnum-1)
	{
		memcpy(info->desptr + pos, info->srcptr + pos, (size_t)info->blocksize);
		printf("Copy pthread [%d] Start [%d] End [%d] Block[%d]\n", (int)pthread_self(), pos, pos + info->blocksize, info->blocksize);
	}
	else
	{
		memcpy(info->desptr + pos, info->srcptr + pos, (size_t)(info->filesize - (info->pthreadnum-1) * info->blocksize));
		printf("Copy pthread [%d] Start [%d] End [%d] Block[%d]\n", (int)pthread_self(), pos, info->filesize, info->filesize - (info->pthreadnum-1)*info->blocksize);
	}

}

void Display_Progress(Info *pInfo, int nCount)
{
	double complete = (double)(pInfo->blocksize * (nCount + 1)) / (double)pInfo->filesize;
	printf("%.2f%%\n", complete*100);
}

int pTh_Create(int pthreadnum, Info *pInfo)
{
	int nCount;
	int err;
	Info *pinfo = NULL;
	//罪魁祸首，罪魁祸首啊！！总结：定义数组要明确分配空间！
	pthread_t *tid = (pthread_t *)malloc(sizeof(pthread_t) * pthreadnum);

	//https://blog.csdn.net/u010299133/article/details/103481141
	//我kao， 程序段错误 __memcpy_sse2_unaligned ()， 竟是因为没能正确传递线程参数！！！！！！！！！，如下所示，引以为戒
	//err = pthread_create(&tid[nCount], NULL, threadjob, (void *)pInfo);
	//需要创建出该函数传入参数的对象，再进行传参？？Why?
	for(nCount = 0; nCount < pthreadnum; nCount++)
	{
		//参数会覆盖，只会是最后一次传递的数，怎么办？
		//原理：因为传入线程的参数是地址，而创建线程很快
		//当线程读取地址中的值时，nNum已经为最大值了
		//解决，传入不同地址,或者指标不治本，让主线程歇一会
		//会造成野指针，如何回收？定义指针记录每一个开辟的空间？
		pinfo = (Info *)malloc(sizeof(Info));
		memcpy(pinfo, pInfo, sizeof(Info));
		pinfo->nNum = nCount;
		err = pthread_create(&tid[nCount], NULL, threadjob, (void *)pinfo);
		//sleep(5);
		if(err > 0)
		{
			printf("call failed error: %s\n", strerror(err));
			return -1;
		}
		else
		{
			printf("Thread [%d] has been create\n", nCount);
		}
	}

	//主线程需等待回收（即阻塞，不然主线程走完结束，其余线程也结束）
	//即创建出来的线程并没有来得及进行工作？？
	//主线程必须等到所有线程copy完成后才能退出！！
	for(nCount = 0; nCount < pthreadnum; nCount++)
	{
		//???????为什么？不接收参数会在主函数结束后报段错误
		//return的原因？？？？？
		//详见代码最后的注释
		pthread_join(tid[nCount], NULL);
		printf("pthread [%d] has been join\n", nCount);
		Display_Progress(pinfo, nCount);
	}
	
	//解除映射！
	munmap(pInfo->srcptr, pInfo->filesize);
	munmap(pInfo->desptr, pInfo->filesize);
}

int main(int argc, char **argv)
{
	int sfd, dfd, filesize, pthreadnum;
	void *Srcptr=NULL, *Desptr=NULL;
	Info *info = (Info *)malloc(sizeof(Info));

	if(argc < 4)
		pthreadnum = 5;
	else
		pthreadnum = atoi(argv[3]);
	block_cut(argv[1], pthreadnum, info);
	info->pthreadnum = pthreadnum;

	sfd = open(argv[1], O_RDONLY);
	if(sfd == -1)
	{
		perror("MainA Open Failed\n");
		return -1;
	}
	dfd = open(argv[2], O_RDWR|O_CREAT, 0775);
	if(dfd == -1)
	{
		perror("MainB Open Faied\n");
		return -1;
	}
	
	//kao，当目标文件创建为新文件时,其大小为0字节. 
	//memcpy会崩溃,因为它试图写入超出文件末尾的数据（总线错误）
	//可以通过在mmap()之前将目标文件预先调整为源文件的大小(使用ftruncate())
	//将新文件的大小设置为与原文件一致
	ftruncate(dfd, info->filesize);

	Srcptr = mmap(NULL, info->filesize, PROT_READ, MAP_SHARED, sfd, 0);
	if(Srcptr == MAP_FAILED)
	{
		perror("MMAPA Failed\n");
		return 0;
	}
	info->srcptr = Srcptr;
	Desptr = mmap(NULL, info->filesize, PROT_READ|PROT_WRITE, MAP_SHARED, dfd, 0);
	if(Desptr == MAP_FAILED)
	{
		perror("MMAPB Failed\n");
		return 0;
	}
	info->desptr = Desptr;
	
	pTh_Create(pthreadnum, info);

	close(sfd);
	close(dfd);
	//使用return 退出线程时，当线程数超过5时会报错？？！！！！
	//pthread_exit(0), 线程到达10会报错？？？？？！！！
	//哈哈，I Know，妙啊，大体来说主函数return后出错
	//不可能是return 导致的，应该在于对数组之类的局部变量修改越界
	//导致栈里的返回地址错误，表现出来的就是return后程序跑飞了！！
	//https://blog.csdn.net/Freedom_Long/article/details/78124836
	//GDB调试也显示栈内的地址出错
	//我根据以上信息，发现自身问题为，调用的pTh_Create函数里创建了
	//pthread_t 类型的数组，用来记录线程id，但没有为其分配大小
	//线程数量过多时，可能导致它越界？？！！
	//一个问题，为什么return和pthread_exit 段错误的临界值不一样？
	//为什么在那个函数内加了个变量就好了？
	return 0;
}
