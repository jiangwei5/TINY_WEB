#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/stat.h>

#define USER_LIMIT 5
#define BUFFER_SIZE 1024
#define FD_LIMIT 65534
#define MAX_EVENT_NUMBER 1024
#define PROCESS_LIMIT 65536

/* ����һ���ͻ����ӱ�Ҫ������ */
struct client_data
{
	sockaddr_in address;
	int connfd;
	pid_t pid;
	int pipefd[2];
}

static const char* shm_name = "/my_shm";
int sig_pipefd[2];
int epollfd;
int listenfd;
int shmfd;
char* share_mem = 0;

/* �ͻ��������顣�����ÿͻ����ӵı��������������飬����ȡ����صĿͻ���������*/
client_data* users = 0;
/* �ӽ��̺Ϳͻ����ӵ�ӳ���ϵ�����ý��̵�PID������������飬����ȥ�ĸý����������Ŀͻ����ӵı�� */
int* sub_process = 0;
/* ��ǰ�ͻ����� */
int user_count = 0;
bool stop_child = false;

int setnonblocking(int fd)
{
	int old_option = fcntl(fd, F_GETFL);
	int new_option = old_option | O_NONBLOCK;
	fcntl(fd, F_SETFL, new_option);
	return old_option;
}

void addfd(int epollfd, int fd)
{
	epoll_event event;
	event.data.fd = fd;
	event.events = EPOLLIN | EPOLLET;
	epoll_ctl(epollfd, EPOLL_CTL_ADD, FD, &event);
	setnonblocking(fd);
}

void sig_handler(int sig)
{
	int save_errno = errno;
	int msg = sig;
	send(sig_pipefd[1], (char*) & msg, 1, 0);
	errno = save_errno;
}

void addsig(int sig, void(* handler)(int), bool restart = true)
{
	struct sigaction sa;
	memset(&sa, '\0', sizeof(sa));
	sa.sa_handler = handler;
	if(restart)
	{
		sa.sa_flag |= SA_RESTART;
	}
	sigfillset(&sa.sa_mask);
	assert(sigaction(sig, &sa, NULL) != -1);
}

void del_resource()
{
	close(sig_pipefd[0]);
	close(sig_pipefd[1]);
	close(listenfd);
	close(epollfd);
	shm_unlink(shm_name);
	delete[] users;
	delete[] sub_process;
}

/* ֹͣһ���ӽ���*/
void child_term_handler(int sig)
{
	stop_child = true;
}

/* �ӽ������еĺ���������idxָ�����ӽ��̴����Ŀͻ����ӵı�ţ�users�Ǳ������пͻ��������ݵ����ݣ� 
����share_memָ�������ڴ����ʼ��ַ*/
int run_child(int idx, client_data* users, char* share_mem)
{
	epoll_event events[MAX_EVENT_NUMBER];
	/* �ӽ���ͬʱ��I/O���ü�����ͬʱ���������ļ��������� �ͻ�����socket���븸����ͨ�ŵĹܵ��ļ������� */
	int child_epollfd = epoll_create(5);
	assert(child_epollfd != -1);
	int connfd = users[idx].connfd;
	addfd(child_epollfd, connfd);
	int pipefd = users[idx].pipefd[1];
	addfd(child_epollfd, pipefd);
	/* �ӽ�����Ҫ�����Լ����źŴ������� */
	addsig(SIGTERM, child_term_handler, false);

	while(!stop_child)
	{
		int number = epoll_wait(child_epollfd, events, MAX_EVENT_NUMBER, -1);
		if((number < 0) && (errno != EINTR))
		{
			printf("epoll failure\n");
			break;
		}

		for(int i = 0; i < number; i++)
		{
			int sockfd = events[i].data.fd;
			/* ���ӽ��̸���Ŀͻ����������ݵ��� */
			if((sockfd == connfd) && (events[i].events & EPOLLIN))
			{
				memset(share_mem + idx* BUFFER_SIZE, '\0', BUFFER_SIZE);
				/* ���ͻ����ݶ�ȡ����Ӧ�Ķ������С� �ö������ǹ����ڴ��һ�Σ�����ʼ��idx* BUFFER_SIZE��
				�� ����ΪBUFFER_SIZE�ֽڡ���ˣ� �����ͻ����ӵĶ������ǹ�����*/
				ret = recv(connfd, share_mem + idx * BUFFER_SIZE, BUFFER_SIZE - 1, 0);
				if(ret < 0)
				{
					if(errno != EAGAIN)
					{
						stop_child = true;
					}
				}
				else if(ret == 0)
				{
					stop_child = true;
				}
				else
				{
					/* �ɹ���ȡ�ͻ����ݺ��֪ͨ�����̣�ͨ���ܵ���������*/
					send(pipefd, (char* )& idx, sizeof(idx), 0)
				}
			}
			/* ������֪ͨ�����̣�ͨ���ܵ��� ����client���ͻ������ݷ��͵������Ǹ���Ŀͻ���*/
			else if((sockfd == pipefd) && (events[i].events & EPOLLIN))
			{
				int client = 0;
				ret = recv(sockfd, (char*) &client, sizeof(client), 0);
				if(ret < 0)
				{
					if(errno != EAGAIN)
					{
						stop_child = true;
					}
					else if(ret == 0)
					{
						stop_child = true;
					}
					else 
					{
						send(connfd , share_mem + client * BUFFER_SIZE, BUFFER_SIZE, 0);
					}
				}
			}
			else
			{
				continue;
			}
		}
	}
	close(connfd);
	close(pipefd);
	close(child_epollfd);
	return 0;
}



int main(int argc, char * argv [])
{
	if(argc <= 2)
	{
		printf("usuage: %s ip_address port_number\n", basename(argv[0]));
		return 1;
	}
	const char* ip = argv[1];
	int port = atoi(argv[2]);

	int ret = 0;
	struct sockaddr_in address; 			// ����IPV4
	bzero(&address, sizeof(address));
	address.sin_family = AF_INET;		// ��ַ���Э�����Ӧ
	inet_pton(AF_INET, ip, &address.sin_addr); // �ѵ��ʮ����ip��ַת���������ֽ����ip��ַ
	address.sin_port = htons(port);		// �����ֽ���->�����ֽ���С��->��ˣ�

	listenfd = socket(PF_INET, SOCK_STREAM, 0);
	assert(listenfd >= 0);

	ret = bind(listenfd, (struct sockaddr*) &address, sizeof(address));		// ����
	assert(ret != -1);

	ret = listen(listenfd, 5);
	assert(ret != -1);

	user_count = 0;
	users = new client_data[USER_LIMIT + 1];
	sub_process = new int [PROCESS_LIMIT];
	for(int i = 0; i < PROCESS_LIMIT; ++i)
	{
		sub_process[i] = -1;
	}

	epoll_event events[MAX_EVENT_NUMBER];
	epollfd = epoll_create(5);
	assert(epollfd != -1);
	addfd(epollfd, listenfd);

	ret = socketpair(PF_UNIX, SOCK_STREAM, 0, sig_pipefd);
	assert(ret != -1);
	setnonblocking(sig_pipefd[1]);
	addfd(epollfd, sig_pipefd[0]);

	addsig(SIGCHLD, sig_handler);
	addsig(SIGTERM, sig_handler);
	addsig(SIGINT, sig_handler);
	addsig(SIGPIPE, SIG_IGN);
	bool stop_server = false;
	bool terminate = false;

	/* ���������ڴ棬��Ϊ���пͻ�socket���ӵĶ����� */
	shmfd = shm_open(shm_name, O_CREATE | O_RDWR, 0666);
	assert(shmfd != -1);
	ret = ftruncatr(shmfd, USER_LIMIT * BUFFER_SIZE);
	assert(ret != -1);

	share_mem = (char*) mmp(NULL, USER_LIMIT* BUFFER_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, shmfd�� 0)��
	assert(shmfd);

	while(!stop_server)
	{
		int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
		if((number < 0) && (errno != EINTR))
		{
			printf("epoll failure\n");
			break;
		}
		for(int i = 0; i < number; i++)
		{
			int sockfd = events[i].data.fd;
			/* �µĿͻ����ӵ��� */
			if(sockfd == listenfd)
			{
				struct sockaddr_in client_address;
				socklen_t client_addrlength = sizeof(client_address);
				int connfd = accept(listenfd, (struct sockaddr*)& client_address, &client_addrlength);

				if(confd < 0)
				{
					printf("errno is : %d\n", errno);
					continue;
				}
				if(user_count >= USER_LIMIT)
				{
					const char* info = "too many users\n";
					printf("%s", info);
					send(connfd, info, strlen(info), 0);
					close(confd);
					continue;
				}
				/* ����user_count ���ͻ����ӵ�������� */
				user[user_count].address = client_address;
				users[user_count].connfd = connfd;
				/* �������̺��ӽ��̼佨���ܵ��� �Դ��ݱ�Ҫ������ */
				ret = socketpair(PF_UNIX, SOCK_STREAM, 0, user[user_count].pipefd);
				assert(ret != -1);
				pid_t pid = fork();
				if(pid < 0)
				{
					close(connfd);
					continue;
				}
				else if(pid == 0)
				{
					close(epollfd);
					close(listenfd);
					close(users[user_count].pipefd[0]);
					close(sig_pipefd[0]);
					close(sig_pipefd[1]);
					run_child(user_count, users, share_mem);
					munmap((void*)share_mem, USER_LIMIT* BUFFER_SIZE);
					exit(0);
				}
				else
				{
					close(connfd);
					close(users[user_count].pipefd[1]);
					addfd(epollfd, users[user_count].pipefd[0]);
					user[user_count].pid = pid;
					/* ��¼�µĿͻ�����������users�е�����ֵ����������pid�͸�����ֵ֮���ӳ���ϵ */
					sub_process[pid] = user_count;
					user_count++;
				}
			}
			else if((sockfd == sig_pipefd[0]) && (events[i].events & EPOLLIN))
			{
				int sig;
				char signals[1024];
				ret = recv(sig_pipefd[0], signals, sizeof(signals), 0);
				if(ret == -1)
				{
					continue;
				}
				else if(ret == 0)
				
				{
					continue;
				}
				else
				{
					for(int i = 0; i < ret; ++i)
					{
						switch(signals[i])
						{
							/* �ӽ����˳��� ��ʾ��ĳ���ͻ��˹ر������� */
							case SIGCHLD��
							{
								pid_t pid;
								int stat;
								while((pid = waitpid(-1, &stat, WNOHANG)) > 0)
								{
									/* ���Ͻ���pid ȥ�ı��رյĿͻ����ӵı�� */
									int del_user = sub_process[pid];
									sub_process[pid] = -1;
									if((del_user < 0) || (del_user > USER_LIMIT))
									{
										continue;
									}
									/* �����del_user ���ͻ�����ʹ�õ�������� */
									epoll_ctl(epollfd, EPOLL_CTL_DEL, users[del_user].pipefd[0], 0);
									close(users[del_user].pipefd[0]);
									users[del_user] = users[--user_count];
									sub_process[users[del_user].pid] =  del_user;
								}
								if(terminate && user_count == 0)
								{
									stop_server = true;
								}
								break;
							}
							case SIGTERM:
							case SIGINT:
							{
								/* �������������� */
								printf("kill all the child now\n");
								if(user_count == 0)
								{
									stop_server = true;
									break;
								}
								for(int i = 0; i < user_count; ++i)
								{
									int pid = users[i].pid;
									kill(pid, SIGTERM);
								}
								terminate = true;
								break;
							}
							default:
							{
								break;
							}
						}
					}
				}
			}
			else if(events[i].events & EPOLLIN)
			{
				int child = 0;
				/* ��ȡ�ܵ����ݣ� child ������¼�����ĸ��ͻ����������ݵ��� */
				ret = recv(sockfd, (char*)& child, sizeof(child), 0);
				printf("read data from child accross pipe\n");
				if(ret == -1)
				{
					continue;
				}
				else if(ret == 0)
				{
					continue;
				}
				else
				{
					/*�����������child ���ͻ����ӵ��Ͻ���֮��������ӽ��̷�����Ϣ��֪ͨ�����пͻ�����Ҫд*/
					for(int j = 0; j < user_count; ++j)
					{
						if(user[j].pipefd[0] != sockfd)
						{
							printf("send data to child accross pipe\n");
							send(users[j].pipefd[0], (char*)&child, sizeof(child), 0);
						}
					}
				}
			}
		}
	}
	del_resource;
	return 0;
}


































