.class Ltest/Launcher; 1
.string runtimeKey "java.runtime.name"
.string android "Android"
.string architectureKey "os.arch"
.string arm "arm"
.field stage:I 2
.method check(I)V 0x109
.end method

.method <init>()V 0x10001
.registers 3
.outs 1
invoke-direct Ljava/lang/Object;-><init>()V, v2
iget v0, v2, Ltest/Launcher;->stage:I
const/4 v1, 1
add-int/2addr v0, v1
iput v0, v2, Ltest/Launcher;->stage:I
invoke-static Ltest/Launcher;->check(I)V, v0
return-void
.end method

.method onCreate(Landroid/os/Bundle;)V 1
.registers 4
.outs 2
if-nez v3, :failed
const-string v0, runtimeKey
invoke-static Ljava/lang/System;->getProperty(Ljava/lang/String;)Ljava/lang/String;, v0
move-result-object v0
const-string v1, android
invoke-virtual Ljava/lang/String;->contains(Ljava/lang/CharSequence;)Z, v0, v1
move-result v0
if-eqz v0, :failed
const-string v0, architectureKey
invoke-static Ljava/lang/System;->getProperty(Ljava/lang/String;)Ljava/lang/String;, v0
move-result-object v0
const-string v1, arm
invoke-virtual Ljava/lang/String;->startsWith(Ljava/lang/String;)Z, v0, v1
move-result v0
if-eqz v0, :failed
iget v0, v2, Ltest/Launcher;->stage:I
const/4 v1, 1
add-int/2addr v0, v1
iput v0, v2, Ltest/Launcher;->stage:I
invoke-static Ltest/Launcher;->check(I)V, v0
return-void
:failed
const/4 v0, -1
invoke-static Ltest/Launcher;->check(I)V, v0
return-void
.end method

.method onStart()V 1
.registers 3
.outs 1
iget v0, v2, Ltest/Launcher;->stage:I
const/4 v1, 1
add-int/2addr v0, v1
iput v0, v2, Ltest/Launcher;->stage:I
invoke-static Ltest/Launcher;->check(I)V, v0
return-void
.end method

.method onResume()V 1
.registers 3
.outs 1
iget v0, v2, Ltest/Launcher;->stage:I
const/4 v1, 1
add-int/2addr v0, v1
iput v0, v2, Ltest/Launcher;->stage:I
invoke-static Ltest/Launcher;->check(I)V, v0
return-void
.end method

.method onPause()V 1
.registers 3
.outs 1
iget v0, v2, Ltest/Launcher;->stage:I
const/4 v1, 1
add-int/2addr v0, v1
iput v0, v2, Ltest/Launcher;->stage:I
invoke-static Ltest/Launcher;->check(I)V, v0
return-void
.end method

.method onStop()V 1
.registers 3
.outs 1
iget v0, v2, Ltest/Launcher;->stage:I
const/4 v1, 1
add-int/2addr v0, v1
iput v0, v2, Ltest/Launcher;->stage:I
invoke-static Ltest/Launcher;->check(I)V, v0
return-void
.end method

.method onDestroy()V 1
.registers 3
.outs 1
iget v0, v2, Ltest/Launcher;->stage:I
const/4 v1, 1
add-int/2addr v0, v1
iput v0, v2, Ltest/Launcher;->stage:I
invoke-static Ltest/Launcher;->check(I)V, v0
return-void
.end method
