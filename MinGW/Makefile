all: cp_dep BonDriver_Tunnel.dll BonDriverTunnel.exe
clean: BonDriver_Tunnel.dll.clean BonDriverTunnel.exe.clean rm_dep
BonDriver_Tunnel.dll: ../BonDriver_Tunnel/BonDriver_Tunnel.cpp
	$(CXX) -Wall -Wl,-s -Wl,--dynamicbase -Wl,--nxcompat -static-libgcc -static-libstdc++ -shared -DNDEBUG -D_WINDOWS -D_UNICODE -DUNICODE -O2 -o $@ $< -lws2_32
BonDriverTunnel.exe: ../BonDriverTunnel/BonDriverTunnel.cpp ../BonDriverTunnel/BonDriverTunnel.rc
	windres -DNDEBUG -D_WINDOWS -D_UNICODE -DUNICODE -J rc -O coff -o BonDriverTunnel.rc.obj $(word 2, $^)
	$(CXX) -Wall -mwindows -Wl,-s -Wl,--dynamicbase -Wl,--nxcompat -static-libgcc -static-libstdc++ -DNDEBUG -D_WINDOWS -D_UNICODE -DUNICODE -O2 -o $@ $< BonDriverTunnel.rc.obj -lws2_32 -lole32 -lcomctl32
BonDriver_Tunnel.dll.clean:
	$(RM) $(basename $@)
BonDriverTunnel.exe.clean:
	$(RM) $(basename $@)
	$(RM) BonDriverTunnel.rc.obj
cp_dep:
	cp -n $(MINGW_PREFIX)/bin/libwinpthread-1.dll .
rm_dep:
	$(RM) libwinpthread-1.dll
