#include <thread>

class Task
{
	typedef void (*pFnTaskProc)(size_t);
public:
	Task(int priority, int stackSize) {
		m_pThread = NULL;
	}
	~Task() {
		if (m_pThread) {
			m_pThread->detach();
			delete m_pThread;
		}
	}
	void create(pFnTaskProc func, size_t arg = 0) {
		m_pThread = new std::thread(func, arg);
	}
private:
	std::thread* m_pThread;
};

#include<mutex>
#include<condition_variable>
#include<queue>
#define NO_WAIT	0
#define WAIT_FOREVER -1
class Mailbox
{
public:
	Mailbox(int msgSize, int msgNum) : m_msgSize(msgSize), m_msgNum(msgNum) {}
	~Mailbox() {
		char* delete_msg;
		while (!queue.empty()) {
			delete_msg = queue.front();
			queue.pop();
			delete[] delete_msg;
		}
	}
	bool post(const void* msg, int timeout) {
		std::unique_lock<std::mutex> lck(mtx);
		if (queue.size() >= m_msgNum) {
			lck.unlock();
			return false;
		}
		char* send_msg = new char[m_msgSize];
		memcpy(send_msg, msg, m_msgSize);
		queue.push(send_msg);
		cv.notify_one();
		return true;
	}
	bool pend(void* msg, int timeout) {
		std::unique_lock<std::mutex> lck(mtx);
		switch (timeout)
		{
		case NO_WAIT:
			if (queue.empty()) return false;
			break;
		case WAIT_FOREVER:
			while (queue.empty()) cv.wait(lck);
			break;
		default:
			if (queue.empty()) {
				cv.wait_for(lck, std::chrono::milliseconds(timeout));
				if (queue.empty()) return false;
			} break;
		}
		char* recv_msg = queue.front();
		queue.pop();
		memcpy(msg, recv_msg, m_msgSize);
		delete[] recv_msg;
		return true;
	}
private:
	int 	m_msgSize;
	int 	m_msgNum;
	std::queue<char*> queue;//队列
	std::mutex mtx;//互斥变量
	std::condition_variable cv;//条件变量
};

#include <windows.h>
class Timer
{
	typedef WAITORTIMERCALLBACK pFnTimerFunc;
public:
	Timer(pFnTimerFunc timer_func, size_t arg = 0) {
		m_func = timer_func;
		m_arg = arg;
		m_handle = NULL;
	}
	~Timer() {
		if (m_handle) stop();
	}
	bool start(int timeout, int period = 0) {
		stop();
		return CreateTimerQueueTimer(&m_handle, NULL, m_func,
			(void*)m_arg, timeout, period, NULL);
	}
	bool stop() {
		bool ret = false;
		if (m_handle) {
			ret = DeleteTimerQueueTimer(NULL, m_handle, NULL);
			m_handle = NULL;
		}
		return ret;
	}

private:
	HANDLE     m_handle;
	pFnTimerFunc    m_func;
	size_t          m_arg;
};

#include <map>
#include <list>
using namespace std;
struct Message;
class MethodCallerBase {
public:
	virtual ~MethodCallerBase() = 0;
	virtual void operator()(const Message &msg) = 0;
};
template <typename Obj>
class MethodCaller : public MethodCallerBase {
	typedef void (Obj::*Func)(const Message &);
	Obj* obj;
	Func method;
public:
	MethodCaller(Obj* obj, Func method)
		: obj(obj), method(method) {}
	void operator()(const Message& msg) override {
		(obj->*method)(msg);
	}
};
typedef list<MethodCallerBase *> CallList;
typedef map<Mailbox*, CallList> MailboxMap;
typedef map<unsigned short, MailboxMap> CallMap;
void TaskFunction(size_t arg);
struct Message
{
	CallList* callInfo;
	unsigned short flag;
	unsigned short length;
	unsigned char data[1000];
};
CallMap callMap;
class Object
{
public:
	Object(Task* task, Mailbox* mailbox){
		m_task = task; m_mailbox = mailbox; m_parent = 0;
	}
	Object(Object* parent) {//与父对象共享任务与邮箱
		m_parent = parent;
		m_task = parent->m_task;
		m_mailbox = parent->m_mailbox;
	}
	virtual ~Object() {
		delete m_task;
		delete m_mailbox;
	}
	bool start() {
		if (m_parent) return false;
		m_task->create(TaskFunction, (size_t)this);
		return true;
	}
	virtual void handleMsg(const Message& msg) {}//默认的消息处理函数
	virtual void taskLoop() { exec(); }
protected:
	void exec() { //启动消息循环
		Message msg;
		CallList* list;
		CallList::iterator p;
		while (1) {
			m_mailbox->pend(&msg, WAIT_FOREVER);
			list = msg.callInfo;
			if (list) {
				for (p = list->begin(); p != list->end(); ++p) {
					(**p)(msg);
				}
			} else {
				handleMsg(msg);
			}
		}
	}
	bool connect(unsigned short flag, Mailbox* mailbox, MethodCallerBase* caller) {
		callMap[flag][mailbox].push_back(caller);
	}
	template<class Obj>
	bool connect(unsigned short flag, Obj* receiver = 0, void (Obj::* slot)(const Message&) = 0)
	{
		if (receiver == 0) {
			connect(flag, this->m_mailbox, new MethodCaller<Object>(this, &Object::handleMsg));
		} else if (slot == 0) {
			connect(flag, receiver->m_mailbox, new MethodCaller<Object>(receiver, &Object::handleMsg));
		} else {
			connect(flag, receiver->m_mailbox, new MethodCaller<Obj>(receiver, slot));
		}
	}
	bool disconnect(unsigned short flag, Object* receiver) {
		CallMap::iterator p = callMap.find(flag);
		if (p == callMap.end()) {
			return false;
		}
		p->second.erase(receiver->m_mailbox);
		if (p->second.empty()) {
			callMap.erase(p);
		}
		return true;
	}
	void publish(Message& msg) {//发布消息
		CallMap::iterator p = callMap.find(msg.flag);
		if (p == callMap.end()) {
			return;
		}
		MailboxMap::iterator q;
		for (q = p->second.begin(); q != p->second.end(); ++q) {
			msg.callInfo = &q->second;
			q->first->post(&msg, NO_WAIT);
		}
	}

private:
	Mailbox* m_mailbox;
	Task* m_task;
	Object* m_parent;
};
void TaskFunction(size_t arg)
{
	Object* p = (Object*)arg;
	p->taskLoop();
}
