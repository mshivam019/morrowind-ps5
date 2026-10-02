# Bullet 3.17 built with USE_DOUBLE_PRECISION=ON; only OpenMW's two components.
get_filename_component(BULLET_ROOT_DIR "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(BULLET_FOUND TRUE)
set(BULLET_VERSION_STRING "3.17")
set(BULLET_INCLUDE_DIRS "include/bullet")
set(BULLET_LIBRARIES "${BULLET_ROOT_DIR}/lib/libBulletCollision.a;${BULLET_ROOT_DIR}/lib/libLinearMath.a")
