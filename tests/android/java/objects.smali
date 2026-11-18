.string phrase "H\u0000\ud83d\ude00"
.class Ltest/Objects; 1

.method text()Ljava/lang/String; 9
.registers 1
const-string v0, phrase
return-object v0
.end method

.method array(I)[I 9
.registers 2
new-array v0, v1, [I
return-object v0
.end method

.method read([II)I 9
.registers 3
aget v0, v1, v2
return v0
.end method

.method filled()I 9
.registers 3
const/4 v1, 2
new-array v0, v1, [B
fill-array-data v0, :payload
const/4 v1, 1
aget-byte v2, v0, v1
return v2
:payload
.words 0x0300, 1, 2, 0, 0x8064
.end method

.method cast()V 9
.registers 1
const-string v0, phrase
check-cast v0, [I
return-void
.end method

.method probe(Ljava/lang/Object;)I 9
.registers 2
instance-of v0, v1, Ljava/lang/Object;
return v0
.end method

.method add([I)V 9
.registers 5
monitor-enter v4
monitor-enter v4
const/4 v0, 0
aget v1, v4, v0
const/16 v2, 500
const/4 v3, 1
:loop
add-int/2addr v1, v3
sub-int/2addr v2, v3
if-nez v2, :loop
aput v1, v4, v0
monitor-exit v4
monitor-exit v4
return-void
.end method
