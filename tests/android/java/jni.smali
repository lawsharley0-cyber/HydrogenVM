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

.method write([II)I 9
.registers 4
const/4 v0, 0
aget v1, v2, v0
aput v3, v2, v0
return v1
.end method

.method newInts()[I 9
.registers 1
const/4 v0, 5
filled-new-array [I, v0
move-result-object v0
return-object v0
.end method

.method store([Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String; 9
.registers 4
const/4 v0, 0
aget-object v1, v2, v0
aput-object v3, v2, v0
return-object v1
.end method

.method same(Ljava/lang/String;Ljava/lang/String;)I 9
.registers 3
const/4 v0, 0
# if-ne v1, v2, :different
.words 0x2133, 0x0003
const/4 v0, 1
:different
return v0
.end method

.class Ltest/Throwing; 1
.method <init>()V 0x10001
.registers 3
const/4 v0, 1
const/4 v1, 0
div-int v0, v0, v1
return-void
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

.string item "list value"
.class Ltest/Collections; 1

.method check()I 9
.registers 5
.outs 3
const/4 v0, 7
const/4 v1, -2
const/4 v2, 4
filled-new-array [I, v0, v1, v2
move-result-object v3
invoke-static Ljava/util/Arrays;->sort([I)V, v3
const/4 v0, 2
invoke-static Ljava/util/Arrays;->copyOf([II)[I, v3, v0
move-result-object v3
const/4 v0, 0
aget v0, v3, v0

new-instance v1, Ljava/util/ArrayList;
invoke-direct Ljava/util/ArrayList;-><init>()V, v1
const-string v2, item
invoke-interface Ljava/util/List;->add(Ljava/lang/Object;)Z, v1, v2
invoke-interface Ljava/util/List;->size()I, v1
move-result v4
const/4 v3, 0
invoke-interface Ljava/util/List;->get(I)Ljava/lang/Object;, v1, v3
move-result-object v2
check-cast v2, Ljava/lang/String;
add-int v0, v0, v4
return v0
.end method

.string next "next value"
.method mapCheck()I 9
.registers 5
.outs 3
new-instance v0, Ljava/util/HashMap;
invoke-direct Ljava/util/HashMap;-><init>()V, v0
const/4 v1, 0
const-string v2, item
const-string v3, next
invoke-interface Ljava/util/Map;->put(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;, v0, v1, v2
move-result-object v4
if-nez v4, :fail
invoke-interface Ljava/util/Map;->put(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;, v0, v1, v3
move-result-object v4
if-ne v4, v2, :fail
invoke-interface Ljava/util/Map;->remove(Ljava/lang/Object;)Ljava/lang/Object;, v0, v1
move-result-object v4
if-ne v4, v3, :fail
invoke-interface Ljava/util/Map;->containsKey(Ljava/lang/Object;)Z, v0, v1
move-result v4
if-nez v4, :fail
invoke-interface Ljava/util/Map;->size()I, v0
move-result v4
return v4
:fail
const/4 v4, -1
return v4
.end method

.method orderCheck()I 9
.registers 5
.outs 3
new-instance v0, Ljava/util/ArrayList;
invoke-direct Ljava/util/ArrayList;-><init>()V, v0
const-string v1, item
const-string v2, next
invoke-interface Ljava/util/List;->add(Ljava/lang/Object;)Z, v0, v1
invoke-interface Ljava/util/List;->add(Ljava/lang/Object;)Z, v0, v2
invoke-interface Ljava/util/List;->add(Ljava/lang/Object;)Z, v0, v2
invoke-static Ljava/util/Collections;->reverse(Ljava/util/List;)V, v0
const/4 v3, 0
const/4 v4, 2
invoke-interface Ljava/util/List;->get(I)Ljava/lang/Object;, v0, v4
move-result-object v4
if-ne v4, v1, :fail
const/4 v4, 2
invoke-static Ljava/util/Collections;->swap(Ljava/util/List;II)V, v0, v3, v4
invoke-interface Ljava/util/List;->get(I)Ljava/lang/Object;, v0, v3
move-result-object v4
if-ne v4, v1, :fail
const/4 v4, 1
return v4
:fail
const/4 v4, -1
return v4
.end method

.method setCheck()I 9
.registers 4
.outs 2
new-instance v0, Ljava/util/HashSet;
invoke-direct Ljava/util/HashSet;-><init>()V, v0
const/4 v1, 0
invoke-interface Ljava/util/Set;->add(Ljava/lang/Object;)Z, v0, v1
move-result v3
if-eqz v3, :fail
invoke-interface Ljava/util/Set;->add(Ljava/lang/Object;)Z, v0, v1
move-result v3
if-nez v3, :fail
invoke-interface Ljava/util/Set;->contains(Ljava/lang/Object;)Z, v0, v1
move-result v3
if-eqz v3, :fail
const-string v2, item
invoke-interface Ljava/util/Set;->add(Ljava/lang/Object;)Z, v0, v2
move-result v3
if-eqz v3, :fail
invoke-interface Ljava/util/Set;->remove(Ljava/lang/Object;)Z, v0, v1
move-result v3
if-eqz v3, :fail
invoke-interface Ljava/util/Set;->contains(Ljava/lang/Object;)Z, v0, v1
move-result v3
if-nez v3, :fail
invoke-interface Ljava/util/Set;->size()I, v0
move-result v3
return v3
:fail
const/4 v3, -1
return v3
.end method
