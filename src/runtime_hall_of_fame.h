#ifndef RUNTIME_HALL_OF_FAME_H
#define RUNTIME_HALL_OF_FAME_H
void runtime_hof_open(void);
int runtime_hof_take_open(void);
const char *runtime_hof_project(const char *name);
void runtime_hof_close(void);
int runtime_hof_active(void);
void runtime_hof_season_changed(int season);
void runtime_hof_update(void *root, void *project);
int runtime_hof_input(void *placeable, unsigned signal);
#endif
