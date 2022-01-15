#include <stdio.h>
#include <string.h>
#include "Server/server.h"
#include "Client/client.h"
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char *argv[]) {
    printf("Good morning\n");
    printf("Checking arguments...\n");

    if(argc < 2){
        printf("Not enough arguments");
        return 0;
    }

    if(strcmp(argv[1],"server") == 0 && argc ){
        printf("Start as server\n");
        if(argc < 3){
            printf("Not enough arguments for server");
            return 0;
        }
        execute_server(argv[2], argv[3]);
    }else{
        printf("Start as client\n");
        //execute_client(argc, argv);
        char *arr = malloc(1000*sizeof(char));
        if (argc < 4) {
       	   char *boilerplate = "/usr/bin/wget -P ./ -q -p --header=\"User-Agent: Mozilla/5.0 (Macintosh; Intel Mac OS X 10_10_0) AppleWebKit/600.1.17 (KHTML, like Gecko) Version/8.0 Safari/600.1.17\" -e robots=off localhost:8085";
       	   strcat(arr, boilerplate);
       	   strcat(arr, argv[argc - 1]);
    	   system(arr);
    	} else {
           char *boilerplate = "/usr/bin/wget -P ./ -q localhost:8085";
           strcat(arr, boilerplate);
       	   strcat(arr, argv[argc - 1]);
    	   system(arr);
        }
    }

    printf("Bye\n");
    return 0;
}
