.class Ltest/Bridge; 1

.method add(I)I 0x109
.end method

.method registered(I)I 0x109
.end method

.method step(I)I 9
.registers 3
.outs 1
invoke-static Ltest/Bridge;->add(I)I, v2
move-result v0
invoke-static Ltest/Bridge;->registered(I)I, v0
move-result v0
invoke-static Landroid/view/InputDevice;->getDeviceIds()[I
move-result-object v1
array-length v1, v1
add-int v0, v0, v1
sget v1, Landroid/os/Build$VERSION;->SDK_INT:I
add-int v0, v0, v1
return v0
.end method

.method fault()I 9
.registers 2
const/4 v0, 1
const/4 v1, 0
div-int v0, v0, v1
return v0
.end method

.class Ltest/JniWorker; 1
.super Ljava/lang/Thread;
.field count:I 9

.method <init>()V 0x10001
.registers 1
.outs 1
invoke-direct Ljava/lang/Thread;-><init>()V, v0
return-void
.end method

.method run()V 1
.registers 5
const-class v0, Ltest/JniWorker;
monitor-enter v0
sget v1, Ltest/JniWorker;->count:I
const/16 v2, 500
const/4 v3, 1
:retry
add-int/2addr v1, v3
sub-int/2addr v2, v3
if-nez v2, :retry
sput v1, Ltest/JniWorker;->count:I
monitor-exit v0
return-void
.end method

.method parallel()I 9
.registers 2
.outs 1
new-instance v0, Ltest/JniWorker;
invoke-direct Ltest/JniWorker;-><init>()V, v0
new-instance v1, Ltest/JniWorker;
invoke-direct Ltest/JniWorker;-><init>()V, v1
invoke-virtual Ljava/lang/Thread;->start()V, v0
invoke-virtual Ljava/lang/Thread;->start()V, v1
invoke-virtual Ljava/lang/Thread;->join()V, v0
invoke-virtual Ljava/lang/Thread;->join()V, v1
sget v0, Ltest/JniWorker;->count:I
return v0
.end method
