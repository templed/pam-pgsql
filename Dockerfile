FROM centos:7

# Mount root of repo here
WORKDIR /tmp/src

RUN yum -y groupinstall 'Development Tools' && \
    yum -y install pam-devel postgresql-devel libgcrypt-devel cmake && \
    mkdir -p /tmp/build

CMD cd /tmp/build && cmake /tmp/src && make -j 10
