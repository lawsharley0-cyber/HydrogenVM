.class Ltest/RandomChecks; 1
.super Ljava/lang/Object;

.method newSeeded(J)Ljava/util/Random; 9
.registers 3
.outs 3
new-instance v0, Ljava/util/Random;
invoke-direct Ljava/util/Random;-><init>(J)V, v0, v1, v2
return-object v0
.end method

.method newDefault()Ljava/util/Random; 9
.registers 1
.outs 1
new-instance v0, Ljava/util/Random;
invoke-direct Ljava/util/Random;-><init>()V, v0
return-object v0
.end method

.method setSeed(Ljava/util/Random;J)V 9
.registers 3
.outs 3
invoke-virtual Ljava/util/Random;->setSeed(J)V, v0, v1, v2
return-void
.end method

.method nextInt(Ljava/util/Random;)I 9
.registers 2
.outs 1
invoke-virtual Ljava/util/Random;->nextInt()I, v1
move-result v0
return v0
.end method

.method nextIntBound(Ljava/util/Random;I)I 9
.registers 3
.outs 2
invoke-virtual Ljava/util/Random;->nextInt(I)I, v1, v2
move-result v0
return v0
.end method

.method nextLong(Ljava/util/Random;)J 9
.registers 3
.outs 1
invoke-virtual Ljava/util/Random;->nextLong()J, v2
move-result-wide v0
return-wide v0
.end method

.method nextBoolean(Ljava/util/Random;)Z 9
.registers 2
.outs 1
invoke-virtual Ljava/util/Random;->nextBoolean()Z, v1
move-result v0
return v0
.end method

.method nextFloat(Ljava/util/Random;)F 9
.registers 2
.outs 1
invoke-virtual Ljava/util/Random;->nextFloat()F, v1
move-result v0
return v0
.end method

.method nextDouble(Ljava/util/Random;)D 9
.registers 3
.outs 1
invoke-virtual Ljava/util/Random;->nextDouble()D, v2
move-result-wide v0
return-wide v0
.end method

.method nextGaussian(Ljava/util/Random;)D 9
.registers 3
.outs 1
invoke-virtual Ljava/util/Random;->nextGaussian()D, v2
move-result-wide v0
return-wide v0
.end method

.method nextBytes(Ljava/util/Random;I)[B 9
.registers 3
.outs 2
new-array v0, v2, [B
invoke-virtual Ljava/util/Random;->nextBytes([B)V, v1, v0
return-object v0
.end method

.method nextBytesNull(Ljava/util/Random;)V 9
.registers 2
.outs 2
const/4 v0, 0
invoke-virtual Ljava/util/Random;->nextBytes([B)V, v1, v0
return-void
.end method
