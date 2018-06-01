

# source the common platform independent functionality and option parsing
script_location=$(cd "$(dirname "$0")"; pwd)
. ${script_location}/common_test.sh

retval=0

# running unittests
# run_unittests --gtest_shuffle \
#              --gtest_death_test_use_fork || retval=1


CVMFS_EXCLUDE=
if [ x"$(lsb_release -cs)" = x"xenial" ]; then
  # Ubuntu 16.04
  # Kernel sources too old for gcc, TODO
  CVMFS_EXCLUDE="src/006-buildkernel"
  # Should work once packages are built on destination platform, TODO
  CVMFS_EXCLUDE="$CVMFS_EXCLUDE src/602-libcvmfs"
  # Expected failure, see test case
  CVMFS_EXCLUDE="$CVMFS_EXCLUDE src/628-pythonwrappedcvmfsserver"

  echo "Ubuntu 16.04... using overlayfs"
  export CVMFS_TEST_UNIONFS=overlayfs
fi
if [ x"$(lsb_release -cs)" = x"trusty" ]; then
  # Ubuntu 14.04
  # aufs, expected failure
  CVMFS_EXCLUDE="src/700-overlayfs_validation src/80*-repository_gateway*"

  echo "Ubuntu 14.04... using aufs instead of overlayfs"
fi
if [ x"$(lsb_release -cs)" = x"precise" ]; then
  # Ubuntu 12.04
  # aufs, expected failure
  CVMFS_EXCLUDE="src/614-geoservice src/700-overlayfs_validation src/80*-repository_gateway*"

  echo "Ubuntu 12.04... using aufs instead of overlayfs"
fi


# cd ${SOURCE_DIRECTORY}/test
# echo "running CernVM-FS client test cases..."
# CVMFS_TEST_CLASS_NAME=ClientIntegrationTests                                  \
# ./run.sh $CLIENT_TEST_LOGFILE -o ${CLIENT_TEST_LOGFILE}${XUNIT_OUTPUT_SUFFIX} \
#                               -x src/004-davinci                              \
#                                  src/005-asetup                               \
#                                  src/007-testjobs                             \
#                                  src/024-reload-during-asetup                 \
#                                  src/050-configrepo                           \
#                                  $CVMFS_EXCLUDE                               \
#                                  --                                           \
#                                  src/0*                                       \
#                               || retval=1


# if [ x"$(uname -m)" = x"x86_64" ]; then
#   echo "running CernVM-FS server test cases..."
#   CVMFS_TEST_CLASS_NAME=ServerIntegrationTests                                  \
#   ./run.sh $SERVER_TEST_LOGFILE -o ${SERVER_TEST_LOGFILE}${XUNIT_OUTPUT_SUFFIX} \
#                                 -x src/518-hardlinkstresstest                   \
#                                    src/585-xattrs                               \
#                                    src/600-securecvmfs                          \
#                                    $CVMFS_EXCLUDE                               \
#                                    --                                           \
#                                    src/5*                                       \
#                                    src/6*                                       \
#                                    src/7*                                       \
#                                    src/8*                                       \
#                                 || retval=1
# fi


echo -n "starting the test S3 provider... "
s3_retval=0
test_s3_pid=$(start_test_s3 $TEST_S3_LOGFILE) || { s3_retval=1; retval=1; echo "fail"; }
echo "done ($test_s3_pid)"
create_test_s3_bucket

if [ $s3_retval -eq 0 ]; then
  echo "running CernVM-FS server test cases against the test S3 provider..."
  CVMFS_TEST_S3_CONFIG=$TEST_S3_CONFIG                                      \
  CVMFS_TEST_HTTP_BASE=$TEST_S3_URL                                         \
  CVMFS_TEST_SERVER_CACHE='/srv/cache'                                      \
  CVMFS_TEST_CLASS_NAME=S3ServerIntegrationTests                            \
  ./run.sh $S3_TEST_LOGFILE -o ${S3_TEST_LOGFILE}${XUNIT_OUTPUT_SUFFIX}     \
                            -x src/518-hardlinkstresstest                   \
                               src/519-importlegacyrepo                     \
                               src/522-missingchunkfailover                 \
                               src/523-corruptchunkfailover                 \
                               src/525-bigrepo                              \
                               src/528-recreatespoolarea                    \
                               src/530-recreatespoolarea_defaultkey         \
                               src/537-symlinkedbackend                     \
                               src/538-symlinkedstratum1backend             \
                               src/542-storagescrubbing                     \
                               src/543-storagescrubbing_scriptable          \
                               src/550-livemigration                        \
                               src/563-garbagecollectlegacy                 \
                               src/568-migratecorruptrepo                   \
                               src/571-localbackendumask                    \
                               src/572-proxyfailover                        \
                               src/583-httpredirects                        \
                               src/585-xattrs                               \
                               src/591-importrepo                           \
                               src/594-backendoverwrite                     \
                               src/595-geoipdbupdate                        \
                               src/600-securecvmfs                          \
                               src/605-resurrectancientcatalog              \
                               src/607-noapache                             \
                               src/608-infofile                             \
                               src/610-altpath                              \
                               src/614-geoservice                           \
                               src/622-gracefulrmfs                         \
                               src/626-cacheexpiry                          \
                               src/700-overlayfsvalidation                  \
                               --                                           \
                               src/500*                                     \
                               || retval=1

  echo -n "Stopping the test S3 provider... "
  sudo kill -2 $test_s3_pid && echo "done" || echo "fail"
fi


# # Ubuntu 12.04
# echo "running CernVM-FS migration test cases..."
# CVMFS_TEST_CLASS_NAME=MigrationTests \
# ./run.sh $MIGRATIONTEST_LOGFILE -o ${MIGRATIONTEST_LOGFILE}${XUNIT_OUTPUT_SUFFIX} \
#                                    migration_tests/*                              \
#                                 || retval=1

exit $retval
