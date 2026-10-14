#!../../bin/linux-x86_64/elderberryMCTestIoc
< envPaths

cd "${TOP}"
dbLoadDatabase "dbd/elderberryMCTestIoc.dbd"
elderberryMCTestIoc_registerRecordDeviceDriver pdbbase

iocInit
emcAclMode default-deny                                                                              
emcAclAllow 127.0.0.1
emcSimDebug 1
emcSimStart 5001
# emcSimTerminator cr
