typedef struct _MIInfo MIInfo;

typedef enum {
  MI_MALLOC,
  MI_REALLOC,
  MI_FREE,
} MIOperation;

struct _MIInfo {
  MIOperation operation;
  pid_t  pid;
  void  *old_ptr;
  void  *new_ptr;
  size_t size;
  unsigned int stack_size;
};
