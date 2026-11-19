.class Ltest/Child; 1
.super Ltest/Base;
.implements Ltest/Scorer;
.implements Ljava/util/concurrent/Callable;
.field seen:I 9
.field extra:I 9 = 5

.method <clinit>()V 0x10008
.registers 2
sget v0, Ltest/Base;->order:I
const/4 v1, 2
add-int v0, v0, v1
sput v0, Ltest/Child;->seen:I
return-void
.end method

.method <init>()V 0x10001
.registers 1
.outs 1
invoke-direct Ltest/Base;-><init>()V, v0
return-void
.end method

.method score(I)I 1
.registers 4
.outs 2
invoke-super Ltest/Base;->score(I)I, v2, v3
move-result v0
sget v1, Ltest/Child;->extra:I
add-int v0, v0, v1
return v0
.end method

.method hashCode()I 1
.registers 2
const/16 v0, 71
return v0
.end method

.method call()Ljava/lang/Object; 1
.registers 1
return-object v0
.end method

.method platformDispatch()I 9
.registers 1
.outs 1
new-instance v0, Ltest/Child;
invoke-direct Ltest/Child;-><init>()V, v0
invoke-interface Ljava/util/concurrent/Callable;->call()Ljava/lang/Object;, v0
move-result-object v0
invoke-virtual Ljava/lang/Object;->hashCode()I, v0
move-result v0
return v0
.end method

.method secret()I 2
.registers 2
const/16 v0, 99
return v0
.end method

.method readOrder()I 9
.registers 1
sget v0, Ltest/Child;->seen:I
return v0
.end method

.method throughInterface()I 9
.registers 2
.outs 2
new-instance v0, Ltest/Child;
invoke-direct Ltest/Child;-><init>()V, v0
const/4 v1, 3
invoke-interface Ltest/Scorer;->score(I)I, v0, v1
move-result v0
return v0
.end method

.method throughBase()I 9
.registers 2
.outs 2
new-instance v0, Ltest/Child;
invoke-direct Ltest/Child;-><init>()V, v0
const/4 v1, 2
invoke-virtual Ltest/Base;->score(I)I, v0, v1
move-result v0
return v0
.end method

.method arraySum()I 9
.registers 4
.outs 2
const/4 v0, 6
const/4 v1, 7
filled-new-array [I, v0, v1
move-result-object v2
const/4 v3, 1
aget v0, v2, v3
return v0
.end method

.class Ltest/BadInit; 1
.method <clinit>()V 0x10008
.registers 2
const/4 v0, 1
const/4 v1, 0
div-int v0, v0, v1
return-void
.end method

.method use()V 9
.registers 0
return-void
.end method

.class Ltest/Worker; 1
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
const-class v0, Ltest/Worker;
monitor-enter v0
sget v1, Ltest/Worker;->count:I
const/16 v2, 500
const/4 v3, 1
:retry
add-int/2addr v1, v3
sub-int/2addr v2, v3
if-nez v2, :retry
sput v1, Ltest/Worker;->count:I
monitor-exit v0
return-void
.end method

.method parallel()I 9
.registers 2
.outs 1
new-instance v0, Ltest/Worker;
invoke-direct Ltest/Worker;-><init>()V, v0
new-instance v1, Ltest/Worker;
invoke-direct Ltest/Worker;-><init>()V, v1
invoke-virtual Ljava/lang/Thread;->start()V, v0
invoke-virtual Ljava/lang/Thread;->start()V, v1
invoke-virtual Ljava/lang/Thread;->join()V, v0
invoke-virtual Ljava/lang/Thread;->join()V, v1
sget v0, Ltest/Worker;->count:I
return v0
.end method

.method startThroughSubclass()I 9
.registers 2
.outs 1
new-instance v0, Ltest/Worker;
invoke-direct Ltest/Worker;-><init>()V, v0
invoke-virtual Ltest/Worker;->start()V, v0
invoke-virtual Ltest/Worker;->join()V, v0
sget v0, Ltest/Worker;->count:I
return v0
.end method

.class Ltest/PlatformChild; 1
.super Landroid/app/Activity;

.method <init>()V 0x10001
.registers 1
.outs 1
invoke-direct Ljava/lang/Object;-><init>()V, v0
return-void
.end method

.method callInherited()I 9
.registers 3
.outs 1
new-instance v0, Ltest/PlatformChild;
invoke-direct Ltest/PlatformChild;-><init>()V, v0
invoke-virtual Ltest/PlatformChild;->getFilesDir()I, v0
move-result v1
invoke-virtual Ltest/PlatformChild;->callSuper()I, v0
move-result v2
add-int v0, v1, v2
return v0
.end method

.method callSuper()I 1
.registers 2
.outs 1
invoke-super Ltest/PlatformChild;->onCreate()I, v1
move-result v0
return v0
.end method
