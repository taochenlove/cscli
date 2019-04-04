#ifndef __CSCLI_VTI_H__
#define __CSCLI_VTI_H__

#ifdef __cplusplus
extern "C" {
#endif

#define CSCLI_VTI_BUFSIZ 1536
#define CSCLI_VTI_MAXHIST 12


typedef enum cscli_terminal_stats_e
{
    CSCLI_VTI_NORMAL,
    CSCLI_VTI_CLOSE,
    CSCLI_VTI_MORE,
    CSCLI_VTI_MORELINE,
    CSCLI_VTI_START,
    CSCLI_VTI_CONTINUE

} cscli_terminal_stats_t;



#ifdef __cplusplus
}
#endif

#endif