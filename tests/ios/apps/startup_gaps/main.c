// Link-only oracle for #31; no imported function is called.
extern void getprogname(void);
extern void strsignal(void);
extern void writev(void);
extern void flock(void);
extern void pthread_cancel(void);
extern void protocol_conformsToProtocol(void);
extern void protocol_copyMethodDescriptionList(void);
extern void protocol_copyProtocolList(void);
extern void class_getIvarLayout(void);
extern void mach_port_allocate(void);
extern void mach_port_insert_right(void);
extern void mach_msg(void);
extern void task_swap_exception_ports(void);
extern void thread_swap_exception_ports(void);
extern void vm_map(void);
extern void vm_protect(void);
extern void CFBitVectorCreate(void);
extern void CFBitVectorCreateMutableCopy(void);
extern void CFBitVectorGetBitAtIndex(void);
extern void CFBitVectorSetBitAtIndex(void);
void (* const startupImports[])(void) __attribute__((used)) = {
getprogname,
strsignal,
writev,
flock,
pthread_cancel,
protocol_conformsToProtocol,
protocol_copyMethodDescriptionList,
protocol_copyProtocolList,
class_getIvarLayout,
mach_port_allocate,
mach_port_insert_right,
mach_msg,
task_swap_exception_ports,
thread_swap_exception_ports,
vm_map,
vm_protect,
CFBitVectorCreate,
CFBitVectorCreateMutableCopy,
CFBitVectorGetBitAtIndex,
CFBitVectorSetBitAtIndex
};
int main(void) { return 0; }
