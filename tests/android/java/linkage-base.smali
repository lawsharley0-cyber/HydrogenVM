.class Ltest/Scorer; 0x601
.method score(I)I 0x401
.end method

.class Ltest/Base; 1
.field order:I 9
.field offset:I 1

.method <clinit>()V 0x10008
.registers 1
const/16 v0, 10
sput v0, Ltest/Base;->order:I
return-void
.end method

.method <init>()V 0x10001
.registers 2
.outs 1
invoke-direct Ljava/lang/Object;-><init>()V, v1
const/4 v0, 4
iput v0, v1, Ltest/Base;->offset:I
return-void
.end method

.method score(I)I 1
.registers 3
iget v0, v1, Ltest/Base;->offset:I
add-int v0, v0, v2
return v0
.end method
