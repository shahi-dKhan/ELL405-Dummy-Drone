Well, some of the ideas we have were chatGPT, now I need to think of a more complex layer of that idea.
First of all, what does it mean by android os, does it really mean only app development?
A D-Bus more or less is a common bus through which most of the programs (Inter-Process Communication) occurs in Unix-like operating systems. There is a central hub, where different programs can communicate to each other through a daemon, there are two main buses: 1 is the system bus, a system bus is the one which runs from the beginning of the boot cycle, which mainly contain the system calls, and messages, which would require to interact with the OS, and may require to interact with hardware. A session bus on the other hand is a user-specific bus, which may not handle system calls, it just handles the IPC from one desktop app to another. 

gRPC is more or less a remote procedure call, which can use the function calls of a separate PC, as if they were a part of the same system. REST API is more or less the standard architecture for the HTTP protocol. 
ioctl and sysfs are some other terms which are more or less related to the file systems management in the OS.

there is another thing about the file structures, the file structures are kept in the top level directory structures in the rootfs, 

Now, we prolly have started it. We are at a good project idea. 
