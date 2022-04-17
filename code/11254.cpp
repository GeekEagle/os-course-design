#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/time.h>
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <semaphore.h>
#include <netinet/in.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <time.h>
#include <pthread.h>
#include <sys/prctl.h>
#include <stdbool.h>

#define VERSION 23
#define BUFSIZE 80960
#define ERROR      42
#define LOG        44
#define FORBIDDEN 403
#define NOTFOUND  404

#ifndef SIGCLD
#define SIGCLD SIGCHLD
#endif

struct {
    char *ext; 
    char *filetype;
}extensions [] = {
{"gif", "image/gif" },
{"jpg", "image/jpg" },
{"jpeg","image/jpeg"},
{"png", "image/png" },
{"ico", "image/ico" },
{"zip", "image/zip" },
{"gz",  "image/gz"  },
{"tar", "image/tar" },
{"htm", "text/html" },
{"html","text/html" }, 
{0,0} 
};

long sumrt=0,sumst=0,sumlt=0,sumwt=0;
float avgrt,avgst,avglt,avgwt;
long numr=0,nums=0,numl=0,numw=0;
pthread_mutex_t lockid1 = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cond1 = PTHREAD_COND_INITIALIZER;  
pthread_mutex_t lockid2 = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cond2 = PTHREAD_COND_INITIALIZER; 
pthread_mutex_t lockid3 = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cond3 = PTHREAD_COND_INITIALIZER; 
pthread_mutex_t lockid4 = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cond4 = PTHREAD_COND_INITIALIZER; 

typedef struct { 
    int hit;
    int fd; 
} webparam;

/* queue status and conditional variable*/
typedef struct staconv {
	pthread_mutex_t mutex;
	pthread_cond_t  cond;  /*⽤于阻塞和唤醒线程池中线程*/
	bool status;          /*表示任务队列状态：false 为⽆任务；true 为有任务*/
} staconv;

/*Task*/
typedef struct task {
	struct task* next;            /* 指向下⼀任务   */
	void  (*function)(void* arg);  /*  函数指针   */
	void* arg;                    /*  函数参数指针  */
} task;
/*Task Queue*/
typedef struct taskqueue {
	pthread_mutex_t mutex;/* ⽤于互斥读写任务队列 */
	task* front;     /* 指向队⾸*/
	task* rear;	     /* 指向队尾*/
	staconv* has_jobs;    /* 根据状态，阻塞线程*/
	int  len;     /* 队列中任务个数*/
} taskqueue;
/* Thread */
typedef struct thread {
	int id;	/* 线程 id*/
	pthread_t pthread;  /* 封装的 POSIX 线程*/
	struct threadpool* pool;    /* 与线程池绑定*/
} thread;
/*Thread Pool*/
typedef struct threadpool {
	thread** threads;    /* 线程指针数组*/
	volatile int num_threads;    /*线程池中线程数量*/
	volatile int num_working;  /*⽬前正在⼯作的线程个数*/
	pthread_mutex_t  thcount_lock;          /*线程池锁⽤于修改上⾯两个变量 */
	pthread_cond_t  threads_all_idle;     /*⽤于销毁线程的条件变量*/
	taskqueue  queue; /*任务队列*/
        volatile bool is_alive;   /* 表示线程池是否还存活*/
}threadpool;

void staconv_wait(staconv* s);
void staconv_signal(staconv* s);
void init_taskqueue(taskqueue* queue);
task* take_taskqueue(taskqueue* queue);
void push_taskqueue(taskqueue* queue,task* curtask);
void destroy_taskqueue(taskqueue* queue);
struct threadpool* initTheadPool(int num_threads);
void addTask2ThreadPool(threadpool* pool,task* curtask);
void waitThreadPool(threadpool* pool);
void destoryThreadPool(threadpool* pool);
int getNumofThreadWorking(threadpool* pool);
int create_thread(struct threadpool* pool, struct thread* pthread, int id);
void* thread_do(struct thread* pthread);

void staconv_wait(staconv* s)//p操作
{
    pthread_mutex_lock(&(s->mutex));
    while(s->status<=0)
    {
        pthread_cond_wait(&(s->cond),&(s->mutex));
    }
    s->status=false;
    pthread_mutex_unlock(&(s->mutex));
}
void staconv_signal(staconv* s)//v操作
{
    pthread_mutex_lock(&(s->mutex));
    s->status=true;
    pthread_cond_signal(&(s->cond));
    pthread_mutex_unlock(&(s->mutex));
}

void init_taskqueue(taskqueue* queue)
{     //初始化任务队列
    queue->len=0;   
    pthread_mutex_init(&(queue->mutex),NULL);
    queue->front=NULL;   //队头
    queue->rear=NULL;    //队尾
    queue->has_jobs=(staconv*)malloc(sizeof(staconv));
    staconv* s=queue->has_jobs;  //队列状态
    pthread_mutex_init(&(s->mutex),NULL);
    pthread_cond_init(&(s->cond),NULL);
    s->status=false;
}

task* take_taskqueue(taskqueue* queue)
{    //提取任务
    pthread_mutex_lock(&(queue->mutex));
    task* t=queue->front;
    if(queue->len==1)
    {
        queue->front=NULL;
        queue->rear=NULL;
        queue->len=0;
    }
    else if(queue->len>1)
    {
        queue->front=t->next;
        queue->len-=1;
        staconv_signal(queue->has_jobs);
    }
    pthread_mutex_unlock(&(queue->mutex));
    return t;
}

void push_taskqueue(taskqueue* queue,task* curtask)
{
    pthread_mutex_lock(&(queue->mutex));//任务队列属于临界资源，上锁
    curtask->next=NULL;
    if(queue->len==0){  //队列中没有任务，则头尾都是这个任务节点
        queue->rear=curtask;
        queue->front=curtask;
    }
    else{   //有任务，尾插
        queue->rear->next=curtask;
        queue->rear=curtask;
    }
    queue->len++;
    staconv_signal(queue->has_jobs);  //队列V操作一次
    pthread_mutex_unlock(&(queue->mutex));
}

void destory_taskqueue(taskqueue* queue)
{
    pthread_mutex_destroy(&(queue->mutex));//销毁队列锁
    free(queue->front);  //销毁队列
    pthread_mutex_destroy(&(queue->has_jobs->mutex)); //销毁状态锁
    pthread_cond_destroy(&(queue->has_jobs->cond));  //销毁线程状态
}

/*线程池初始化函数*/
struct threadpool* initThreadPool(int num_threads) {
	//创建线程池空间
	threadpool* pool;
	pool = (threadpool*)malloc(sizeof(struct threadpool));
	pool->num_threads = 0;
	pool->num_working=0; 
	pool->is_alive=true;
    //初始化互斥量和条件变量
    pthread_mutex_init(&(pool->thcount_lock), NULL); 
    pthread_cond_init(&pool->threads_all_idle, NULL); 
    //初始化任务队列
    //****需实现*****
    init_taskqueue(&pool->queue); 
    //创建线程数组
    pool->threads=(struct thread *)malloc(num_threads*sizeof(struct thread*)); 
    //创建线程
    for (int i = 0; i < num_threads; i++) {
        create_thread(pool,&(pool->threads[i]),i);  //i 为线程 id, 
    }
    //等等所有的线程创建完毕,在每个线程运⾏函数中将进⾏ pool->num_threads++ 操作 
    //因此，此处为忙等待，直到所有的线程创建完毕，并⻢上运⾏阻塞代码时才返回。 
    while(pool->num_threads!=num_threads) {}
    return pool; 
}

/*向线程池中添加任务*/
void addTask2ThreadPool(threadpool* pool,task* curtask)
{ 
    //将任务加⼊队列
    push_taskqueue(&pool->queue,curtask); 
}
/*等待当前任务全部运⾏完*/
void waitThreadPool(threadpool* pool){ 
    pthread_mutex_lock(&pool->thcount_lock);
    while (pool->queue.len || pool->num_working) { 
        //队列中有任务，或有线程池中有工作的线程
        pthread_cond_wait(&pool->threads_all_idle, &pool->thcount_lock); 
    }
    pthread_mutex_unlock(&pool->thcount_lock); 
}
/*销毁线程池*/
void destoryThreadPool(threadpool* pool)
{
    //如果当前任务队列中有任务，需等待任务队列为空，并且运⾏线程执⾏完任务后
    if((pool->queue).has_jobs->status==true)
        waitThreadPool(pool);
    //销毁任务队列 
    //****需实现*****
    destory_taskqueue(&pool->queue);
    //销毁线程指针数组,并释放所有为线程池分配的内存
    free(pool->threads);
    pthread_mutex_destroy(&(pool->thcount_lock));
    pthread_cond_destroy(&(pool->threads_all_idle));
}
/*获得当前线程池中正在运⾏线程的数量*/ 
int getNumofThreadWorking(threadpool* pool){ 
    return pool->num_working;
}

/*线程运⾏的逻辑函数*/
void* thread_do(struct thread* pthread)
{ 
    /* 设置线程名字 */
    char thread_name[128] = {0};
    sprintf(thread_name, "thread-pool-%d", pthread->id); 
    prctl(PR_SET_NAME, thread_name);
    /* 获得线程池*/
    threadpool* pool = pthread->pool;
	/* 在线程池初始化时，⽤于已经创建线程的计数，执⾏ pool->num_threads++ */
    pthread_mutex_lock(&(pool->thcount_lock));  
    pool->num_threads++;  
    pthread_mutex_unlock(&(pool->thcount_lock));
    /*线程⼀直循环往复运⾏，直到 pool->is_alive 变为 false*/ 
    while(pool->is_alive){
        /*如果任务队列中还要任务，则继续运⾏，否则阻塞*/
        staconv* s=(pool->queue).has_jobs;
        staconv_wait(s);  //对任务p操作
        if (pool->is_alive){
            /*执⾏到此位置，表明线程在⼯作，需要对⼯作线程数量进⾏计数*/ 
            pthread_mutex_lock(&(pool->thcount_lock));
            pool->num_working++;
            pthread_mutex_unlock(&(pool->thcount_lock));
            /* 从任务队列的队⾸提取任务，并执⾏*/ 
            void (*func)(void*);  //函数指针，指向web()
            void*  arg;  //要传递的变量
            //take_taskqueue 从任务队列头部提取任务，并在队列中删除此任务 
            //****需实现 take_taskqueue*****
            task* curtask = take_taskqueue(&pool->queue); 
            if (curtask) {
                func = curtask->function; 
                arg  = curtask->arg; 
                //执⾏任务
                func(arg);   //执行web
                //释放任务 
                free(curtask);  //任务执行完了，释放task
            }
            /*执⾏到此位置，表明线程已经将任务执⾏完成，需更改⼯作线程数量*/
           //此处还需注意，当⼯作线程数量为 0，表 示 任 务 全 部 完 成 ，要 让 阻 塞 在 waitThreadPool 函数上的线程继续运⾏
           pthread_mutex_lock(&(pool->thcount_lock));
            pool->num_working--;
            if(pool->num_working==0)
                pthread_cond_signal(&(pool->threads_all_idle));
            //没有工作的线程了，销毁的条件变量激活
            pthread_mutex_unlock(&(pool->thcount_lock));
        }
    }
	/*运⾏到此位置表明，线程将要退出，需更改当前线程池中的线程数量*/ 
    //pool->num_threads--
	pthread_mutex_lock(&(pool->thcount_lock)); 
    pool->num_threads--;
    pthread_mutex_unlock(&(pool->thcount_lock));
    return NULL;
}
/*创建线程*/
int create_thread (struct threadpool* pool, struct thread* pthread, int id)
{ 
    //为 thread 分配内存空间
    pthread = (struct thread*)malloc(sizeof(struct thread)); 
    if (pthread == NULL){
        perror("creat_thread(): Could not allocate memory for thread\n"); 
        return -1;
    }
    //设置这个 thread 的属性
	(pthread)->pool = pool; 
    (pthread)->id = id; 
    //创建线程
    pthread_create(&(pthread)->pthread, NULL, (void *)thread_do, (pthread)); 
    pthread_detach((pthread)->pthread);
    return 0; 
}

unsigned long get_file_size(const char *path) {
    unsigned long filesize = -1; struct stat statbuff;
    if(stat(path, &statbuff) < 0)
        return filesize;
    else
        filesize = statbuff.st_size; 
    return filesize; 
}

void logger(int type, char *s1, char *s2, int socket_fd) 
{ 
  int fd ; 
  struct timeval t1,t2;
  double timeuse;
  char logbuffer[BUFSIZE*2]; 
  time_t timep;
  struct tm *p;
  time(&timep);
  p = gmtime(&timep);
  /*根据消息类型，将消息放入logbuffer缓存，或直接将消息通过socket通道返回给客户端*/ 
  gettimeofday(&t1,NULL);
  switch (type) { 
  case ERROR: 
    (void)sprintf(logbuffer,"date:%d\\%d\\%d %d:%d:%d\nERROR: %s:%s Errno=%d exiting pid=%d",(1900+p->tm_year),(1+p->tm_mon),p->tm_mday,p->tm_hour+8,p->tm_min,p->tm_sec,s1, s2, errno,getpid()); 
    break; 
  case FORBIDDEN: 
    (void)write(socket_fd, "HTTP/1.1 403 Forbidden\nContent-Length: 185\nConnection: close\nContent-Type: text/html\n\n<html><head>\n<title>403 Forbidden</title>\n</head><body>\n<h1>Forbidden</h1>\n The requested URL, file type or operation is not allowed on this simple static file webserver.\n</body></html>\n",271); 
    (void)sprintf(logbuffer,"date:%d\\%d\\%d %d:%d:%d\nFORBIDDEN: %s:%s",(1900+p->tm_year),(1+p->tm_mon),p->tm_mday,p->tm_hour+8,p->tm_min,p->tm_sec,s1, s2); 
    break; 
  case NOTFOUND: 
    (void)write(socket_fd,  "HTTP/1.1  404  Not  Found\nContent-Length:  136\nConnection: close\nContent-Type:  text/html\n\n<html><head>\n<title>404  Not Found</title>\n</head><body>\n<h1>Not  Found</h1>\nThe  requested  URL  was  not  found  on  this server.\n</body></html>\n",224); 
    (void)sprintf(logbuffer,"date:%d\\%d\\%d %d:%d:%d\nNOT FOUND: %s:%s",(1900+p->tm_year),(1+p->tm_mon),p->tm_mday,p->tm_hour+8,p->tm_min,p->tm_sec,s1, s2); 
    break; 
  case LOG:
    (void)sprintf(logbuffer,"date:%d\\%d\\%d %d:%d:%d\nINFO: %s:%s:%d",(1900+p->tm_year),(1+p->tm_mon),p->tm_mday,p->tm_hour+8,p->tm_min,p->tm_sec,s1, s2,socket_fd); 
    break; 
  } 
  gettimeofday(&t2,NULL);
  timeuse = (t2.tv_sec-t1.tv_sec)*1000000.0+t2.tv_usec-t1.tv_usec;
  /*printf("Time spent on putting in loggbuffer cache：%lf\n",timeuse);*/
  /* 将logbuffer缓存中的消息存入webserver.log文件*/ 
  gettimeofday(&t1,NULL);
  if((fd = open("nweb.log", O_CREAT| O_WRONLY | O_APPEND,0644)) >= 0) { 
    (void)write(fd,logbuffer,strlen(logbuffer)); 
    (void)write(fd,"\n",1); 
    (void)close(fd); 
  } 
  gettimeofday(&t2,NULL);
  timeuse = (t2.tv_sec-t1.tv_sec)*1000000.0+t2.tv_usec-t1.tv_usec;
  /*printf("The time of loggbuffer sending to webserver.log：%lf\n",timeuse);*/
}

threadpool* read_msg_pool; //初始化线程池300个线程容量，且300个线程开始等待任务队列的进来

void *web(void *data)
{
    int fd;
    int hit;
    int j, file_fd, buflen; 
    long i, ret, len; 
    char * fstr;
    struct timeval tss,tse,tws,twe,tls,tle;
    char buffer[BUFSIZE+1]; /* static so zero filled */ 
    webparam *param=(webparam*) data;
    fd=param->fd; hit=param->hit;
    struct timeval trs,tre;
    /*printf("1111111\n");*/
    long readtime;
    gettimeofday(&trs,NULL);
    ret =read(fd,buffer,BUFSIZE);    /* read web request in one go */ 

    if(ret == 0 || ret == -1) {  /* read failure stop now */
        logger(FORBIDDEN,"failed to read browser request","",fd); 
    }
    else{
        long sendtime;
        gettimeofday(&tss,NULL);
        if(ret > 0 && ret < BUFSIZE)  /* return code is valid chars */ 
            buffer[ret]=0;     /* terminate the buffer */
        else 
            buffer[0]=0;
        for(i=0;i<ret;i++)  /* remove cf and lf characters */ 
            if(buffer[i] == '\r' || buffer[i] == '\n')
                buffer[i]='*';
        logger(LOG,"request",buffer,hit);

        if( strncmp(buffer,"GET ",4) && strncmp(buffer,"get ",4) ) { 
            logger(FORBIDDEN,"only simple get operation supported",buffer,fd);
        }
         /*printf("66666666666\n");*/
        for(i=4;i<BUFSIZE;i++) { /* null terminate after the second space to ignore extra stuff */ 
            if(buffer[i] == ' ') { /* string is "get url " +lots of other stuff */
                buffer[i] = 0; 
                break;
            } 
        }
        for(j=0;j<i-1;j++)    /* check for illegal parent directory use .. */ 
            if(buffer[j] == '.' && buffer[j+1] == '.') {
                logger(FORBIDDEN,"parent directory (..) path names not supported",buffer,fd); 
            }
        if( !strncmp(&buffer[0],"GET /\0",6) || !strncmp(&buffer[0],"get /\0",6) ) /* convert no filename to index file*/
            (void)strcpy(buffer,"GET /index.html");
        /* work out the file type and check we support it */ 
        buflen=strlen(buffer);
        fstr = (char *)0;
        for(i=0;extensions[i].ext != 0;i++){ 
            len = strlen(extensions[i].ext);
            if( !strncmp(&buffer[buflen-len], extensions[i].ext, len)) { 
                fstr =extensions[i].filetype;
                break; 
            }
        }
        if(fstr == 0) 
            logger(FORBIDDEN,"file extension type not supported",buffer,fd); 
        if(( file_fd = open(&buffer[5],O_RDONLY)) == -1) {  /* open the file for reading */
            logger(NOTFOUND, "failed to open file",&buffer[5],fd); 
        }
        long logtime;
        gettimeofday(&tls,NULL);
        logger(LOG,"send",&buffer[5],hit);
        len = (long)lseek(file_fd, (off_t)0, SEEK_END); /* 使⽤ lseek 来获得⽂件⻓度，⽐较低效*/ (void)lseek(file_fd, (off_t)0, SEEK_SET);           /* 想想还有什么⽅法来获取*/
        (void)sprintf(buffer,"HTTP/1.1  200  ok\nserver:  nweb/%d.0\ncontent-length:  %ld\nconnection: close\ncontent-type: %s\n\n", VERSION, len, fstr); /* header + a blank line */
        logger(LOG,"header",buffer,hit); 
        (void)write(fd,buffer,strlen(buffer));

        /* send file in 8kb block - last block may be smaller */ 
        long webreadtime;
        gettimeofday(&tws,NULL);
        while ((ret = read(file_fd, buffer, BUFSIZE)) > 0 ) {
            (void)write(fd,buffer,ret); 
        }
        gettimeofday(&twe,NULL);
        usleep(10000);/*在 socket 通道关闭前，留出⼀段信息发送的时间*/ 
        close(file_fd);
    }
    close(fd); //释放内存 
    free(param);
    printf("The thread%d number is:%lu\n",hit,pthread_self());  
}
int main(int argc, char **argv) 
{
    int i, port, pid, listenfd, socketfd, hit; 
    socklen_t length;
    static struct sockaddr_in cli_addr; /* static = initialised to zeros */ 
    static struct sockaddr_in serv_addr; /* static = initialised to zeros */
    sem_t* psem;
    int show;
    /*if((show=sem_init(psem,0, 1))!=0){ 
        perror("create semaphore error");
        exit(1); 
    }*/
    if( argc < 3  || argc > 3 || !strcmp(argv[1], "-?") ) {
        (void)printf("hint: nweb Port-Number Top-Directory\t\tversion %d\n\n"
                     "\tnweb is a small and very safe mini web server\n"
                    "\tnweb only servers out file/web pages with extensions named below\n" 
                    "\t and only from the named directory or its sub-directories.\n"
                    "\tThere is no fancy features = safe and secure.\n\n" 
                    "\tExample: nweb 8181 /home/nwebdir &\n\n"
                    "\tOnly Supports:", VERSION);
        for(i=0;extensions[i].ext != 0;i++) 
            (void)printf(" %s",extensions[i].ext);
        (void)printf("\n\tNot Supported: URLs including \"..\", Java, Javascript, CGI\n"
                    "\tNot Supported: directories / /etc /bin /lib /tmp /usr /dev /sbin \n"
                    "\tNo warranty given or implied\n\tNigel Griffiths nag@uk.ibm.com\n");
        exit(0); 
    }
    if( !strncmp(argv[2],"/"    ,2 ) || !strncmp(argv[2],"/etc", 5 ) ||
        !strncmp(argv[2],"/bin",5 ) || !strncmp(argv[2],"/lib", 5 ) || 
        !strncmp(argv[2],"/tmp",5 ) || !strncmp(argv[2],"/usr", 5 ) || 
        !strncmp(argv[2],"/dev",5 ) || !strncmp(argv[2],"/sbin",6) ){
          (void)printf("ERROR: Bad top directory %s, see nweb -?\n",argv[2]); exit(3);
    }
    if(chdir(argv[2]) == -1){
        (void)printf("ERROR: Can't Change to directory %s\n",argv[2]); 
        exit(4);
    }
    /* Become deamon + unstopable and no zombies children (= no wait()) */ 
    pid_t pro = fork();
    if (pro==0)
    {
    (void)signal(SIGCLD, SIG_IGN); /* ignore child death */ 
    (void)signal(SIGHUP, SIG_IGN); /* ignore terminal hangups */ 
    logger(LOG,"nweb starting",argv[1],getpid());
    /* setup the network socket */
    if((listenfd = socket(AF_INET, SOCK_STREAM,0)) <0) 
        logger(ERROR, "system call","socket",0);
    port = atoi(argv[1]); 
    if(port < 0 || port >60000)
        logger(ERROR,"Invalid port number (try 1->60000)",argv[1],0); 
    
    //初始化线程属性，为分离状态
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED); 
    pthread_attr_setscope(&attr,PTHREAD_SCOPE_SYSTEM);
    pthread_attr_setschedpolicy(&attr,SCHED_FIFO);
    //
    pthread_t pth; 
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons(port);
    if(bind(listenfd, (struct sockaddr *)&serv_addr,sizeof(serv_addr)) <0) 
        logger(ERROR,"system call","bind",0);
    if( listen(listenfd,64) <0)
        logger(ERROR,"system call","listen",0); 
    printf("The child process number is:%d,its father is%d\n",getpid(),getppid());
    read_msg_pool=initThreadPool(50);//初始化线程池50个线程容量，且300个线程开始等待任务队列的进来
    for(hit=1; ;hit++) {
        length = sizeof(cli_addr);
        if((socketfd = accept(listenfd, (struct sockaddr *)&cli_addr, &length)) < 0) 
            logger(ERROR,"system call","accept",0);
        webparam *param=malloc(sizeof(webparam)); 
        param->hit=hit;
        param->fd=socketfd;
        task* t = (task*)malloc(sizeof(task));
        t->function=(void*)web;
        t->arg=(void*)param;
        addTask2ThreadPool(read_msg_pool,t);//将任务加入解析线程池中
    }
    }
    else if (pro>0){
        close(socketfd);
        printf("Father number is %d\n",getpid());
        wait(NULL);
    }
}