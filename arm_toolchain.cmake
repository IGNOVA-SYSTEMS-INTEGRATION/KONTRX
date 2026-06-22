# تعريف بيئة العمل
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

# الأمر السحري لحل مشكلة اختبار المترجم بدون الاعتماد على nosys هنا
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# تحديد مسارات المترجم
set(CMAKE_C_COMPILER arm-none-eabi-gcc)
set(CMAKE_CXX_COMPILER arm-none-eabi-g++)
set(CMAKE_ASM_COMPILER arm-none-eabi-gcc)