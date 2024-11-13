#include "parser.h" //librería para leer los mandatos por la entrada estándar
#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <ctype.h>
#include <signal.h>

#define MAX_PROCESOS_BACKGROUND 100 //constante para el máximo de comandos que pueden estar ejecutándose en background
#define COLOR_ROSA "\x1b[35m"
#define COLOR_NORMAL "\x1b[0m"

//variables globales
pid_t array_background[MAX_PROCESOS_BACKGROUND];
char array_comandos[MAX_PROCESOS_BACKGROUND][1024];
int stdout_fd; //descriptor original salida
int stderr_fd; //descriptor original error

//declaración de todas las cabeceras
void manejar_sigint();
void manejar_sigchld();
void guardar_descriptores_originales();
void restaurar_descriptores_originales();
void redireccion_entrada(char *input_file);
int redireccion_salida(char *output_file);
int redireccion_error(char *input_file);
void agregar_pid_background(pid_t pid, char comando[1024]);
void eliminar_pid_background(pid_t pid);
int es_proceso_bg(pid_t pid);
void matar_background();
int es_octal(const char *cadena);
void ejecucion_jobs(tline *line);
void ejecucion_exit(tline *line);
void ejecucion_umask(tline *line);
void ejecucion_fg(int pid, tline *line);
int ejecucion_cd(tline *line);

void un_comando(tline *line, char buf[1024]);
void varios_comandos(tline *line);

int main(void) {
    tline *line;
    char buf[1024];
    int i;
    pid_t pid;
    int hay_cd = 0;

	//manejadores de señales
	signal(SIGINT, manejar_sigint);
	signal(SIGCHLD, manejar_sigchld);
	signal(SIGTERM, SIG_DFL);

	//inicializamos background
    for (i = 0; i < MAX_PROCESOS_BACKGROUND; i++) {
		array_background[i] = -101;
		strcpy(array_comandos[i], "NULL");
	}

	system("clear"); //para borrar todo lo que hay en la terminal
	printf("Ejecutando Minishell\n");
	printf(COLOR_ROSA "								MINISHELL CREADA POR LORETO UZQUIANO ESTEBAN\n");
    printf(COLOR_NORMAL "msh> ");
    while (fgets(buf, 1024, stdin)) {
        line = tokenize(buf);
        if (line == NULL) {
            continue;
        }
        if (line->ncommands == 0) {
            printf("msh> ");
            continue;
        }
        if (access(line->commands[0].filename, X_OK) == -1 && strcmp(line->commands[0].argv[0], "exit") != 0 && strcmp(line->commands[0].argv[0], "cd") != 0 && strcmp(line->commands[0].argv[0], "jobs") != 0 && strcmp(line->commands[0].argv[0], "umask") != 0 && strcmp(line->commands[0].argv[0], "fg") != 0) { //comando no existe
            fprintf(stderr, "Error: El comando %s no existe.\n", line->commands[0].filename);
            printf("msh> ");
            continue;
        }
		if ((line->ncommands == 1)) { //1 comando
			if (line->commands[0].argc > 0 && line->commands[0].argv != NULL && strcmp(line->commands[0].argv[0], "exit") == 0) { //exit
                ejecucion_exit(line);
            }
			else if (line->commands[0].argc > 0 && line->commands[0].argv != NULL && strcmp(line->commands[0].argv[0], "cd") == 0) { //cd
                ejecucion_cd(line);
            }
			else if (line->commands[0].argc > 0 && line->commands[0].argv != NULL && strcmp(line->commands[0].argv[0], "jobs") == 0) { //jobs
                ejecucion_jobs(line);
            }
			else if (line->commands[0].argc > 0 && line->commands[0].argv != NULL && strcmp(line->commands[0].argv[0], "umask") == 0) { //umask
				ejecucion_umask(line);
            }
			else if (line->commands[0].argc > 0 && line->commands[0].argv != NULL && strcmp(line->commands[0].argv[0], "fg") == 0) { //fg
                if(line->commands[0].argv[1] != NULL) { //con argumento
                    ejecucion_fg(atoi(line->commands[0].argv[1]), line);
				}
				else { //sin argumento -> último proceso en entrar a background
					ejecucion_fg(-1, line);
				}
            }
			else {
				un_comando(line, buf);
			}
        }
		else if ((line->ncommands > 1)) { //más de 1 comando
			for (i = 0; i < line->ncommands; i++) { //en este for controlo que no esté el comando cd en el pipe
                if (strcmp(line->commands[i].argv[0], "cd") == 0) {
                    fprintf(stderr, "El comando cd no se puede ejecutar con pipes\n");
                    hay_cd = 1;
                    break;
                }
            }

			if (hay_cd == 0) { //si no está el comando cd en el pipe
				if (line->background) { //en BG
                    pid = fork();
                        if (pid < 0) {
                            fprintf(stderr,"Error al ejecutar el comando en background: fork()\n");
                        }
                        else if(pid == 0) {
                            varios_comandos(line);
                            exit(EXIT_SUCCESS);
                        }
                        else {
                            fprintf(stdout,"[%d]\n", pid);
                            agregar_pid_background(pid, buf);
                        }
                }
                else { //en FG
                    varios_comandos(line);
                }
            }
            else { //si está el comando cd en el pipe
                hay_cd = 0;
            }
		}
        printf("msh> ");
    }
	return 0;
}

void manejar_sigint() { //para manejar la señal en FG
    printf("\nmsh> ");
    fflush(stdout);
}

void manejar_sigchld() { //para procesos en background
	pid_t pid;

	while ((pid = waitpid(-1, NULL, WNOHANG)) > 0) {
		if (es_proceso_bg(pid)){
            printf("\n");
            printf("[%d]+ Terminado\n", pid);
			eliminar_pid_background(pid);
		}
	}
}

void guardar_descriptores_originales() {
    stdout_fd = dup(STDOUT_FILENO);
    stderr_fd = dup(STDERR_FILENO);
}

void restaurar_descriptores_originales() {
    dup2(stdout_fd, STDOUT_FILENO);
    dup2(stderr_fd, STDERR_FILENO);
    close(stdout_fd);
    close(stderr_fd);
}

void redireccion_entrada(char *entrada) { //la entrada de esta función es el fichero
	FILE *fichero;
	int fd;

	if (entrada != NULL) { //si hay fichero
        fichero = fopen(entrada, "r"); //abro el fichero

        if (fichero == NULL) {
            fprintf(stderr, "Error: El fichero no se puede abrir\n");
			exit(1);
        }

		fd = fileno(fichero); //fd es el descriptor del fichero -> función fileno

        //redirijo la entrada estandar
        if (dup2(fd, STDIN_FILENO) == -1) {
            fprintf(stderr, "Error en la redirección de entrada\n");
            exit(2);
        }

        close(fd); //cierro el descriptor

        fclose(fichero); //cierro el fichero
    }
}

int redireccion_salida(char *salida) { //la entrada de esta función es el fichero
	FILE *fichero;
    int fd;

    if (salida != NULL) { //si hay fichero
        fichero = fopen(salida, "w"); //abro el fichero

        if (fichero == NULL) {
            fprintf(stderr, "Error: El fichero no se puede abrir\n");
            return 1;
        }

        fd = fileno(fichero); //fd es el descriptor del fichero -> función fileno

        //redirijo la salida estandar
        if (dup2(fd, STDOUT_FILENO) == -1) {
            fprintf(stderr, "Error en la redirección de salida\n");
            return 2;
        }

        close(fd); //cierro el descriptor

        fclose(fichero); //cierro el fichero
    }
	return 0;
}

int redireccion_error(char *error) { //la entrada de esta función es el fichero
	FILE *fichero;
    int fd;

    if (error != NULL) { //si hay fichero
        fichero = fopen(error, "w"); //abro el fichero

        if (fichero == NULL) {
            fprintf(stderr, "Error: El fichero no se puede abrir\n");
            return 1;
        }

        fd = fileno(fichero); //fd es el descriptor del fichero -> función fileno

        //redirijo la salida estandar de error
        if (dup2(fd, STDERR_FILENO) == -1) {
            fprintf(stderr, "Error en la redirección de error\n");
            return 2;
        }

        close(fd); //cierro el descriptor

        fclose(fichero); //cierro el fichero
    }
	return 0;
}

void agregar_pid_background(pid_t pid, char comando[1024]) {
	int i = 0;

	while (i < MAX_PROCESOS_BACKGROUND && array_background[i] != -101) { //mientras que haya hueco en el array
		i++;
	}
	array_background[i] = pid; //añado el pid al array de pids
	strcpy(array_comandos[i], comando); //añado el comando al array de comandos
}

void eliminar_pid_background(pid_t pid) {
	int i = 0;
	int j;

	if (es_proceso_bg(pid) == 1){
		while (i < MAX_PROCESOS_BACKGROUND && array_background[i] != pid) {
			i++;
		}
		for (j = i; j < MAX_PROCESOS_BACKGROUND - 1; j++) {
			array_background[j] = array_background[j + 1];
			strcpy(array_comandos[j], array_comandos[j + 1]);
		}
		array_background[MAX_PROCESOS_BACKGROUND - 1] = -101; //elimino el pid del array de pids
        strcpy(array_comandos[MAX_PROCESOS_BACKGROUND - 1], "NULL"); //elimino el comando del array de comandos
	}
}

int es_proceso_bg(pid_t pid) { //si es un proceso ejecutándose en background, devuelvo 1, sino, devuelvo 0
	int booleano = 0;
	int i = 0;

	while (booleano == 0 && i < MAX_PROCESOS_BACKGROUND) {
		if (array_background[i] == pid){
			booleano = 1;
		}
		i++;
	}
	return booleano;
}

void matar_background() { //para matar procesos en background antes de salir de la minishell
	int i = 0;

	while (i < MAX_PROCESOS_BACKGROUND && array_background[i] != -101) { //mientras haya procesos ejecutándose en background
		kill(array_background[i], SIGTERM);
	}
	//si hay procesos en background saldrá un mensaje por pantalla diciendo "Terminado"
}

int es_octal(const char *cadena) {
    while (*cadena != '\0') {
        if (!isspace(*cadena)) { //para ver si la cadena contiene blancos
            if (*cadena < '0' || *cadena > '7') { //verifica si cada carácter es un dígito octal válido (0-7)
                return 0; //no es octal válido
            }
        }
        cadena++;
    }
    return 1; //sí es octal válido
}

void ejecucion_jobs(tline *line) {
	int i = 0;

	guardar_descriptores_originales(); //para que se pueda redirigir la salida estándar/error con jobs
	if (line->redirect_output != NULL) {
		redireccion_salida(line->redirect_output);
	}
	if (line->redirect_error != NULL) {
		redireccion_error(line->redirect_error);
	}

	if (array_background[0] < 0) {//no hay procesos en background
		printf("No hay procesos ejecutándose en background\n");
		restaurar_descriptores_originales();
		return;
	}
    while (i < MAX_PROCESOS_BACKGROUND && array_background[i] != -101) { //muestra todos los procesos en background
        printf("[%d]+ Running         %s", array_background[i], array_comandos[i]);
        i++;
	}
	restaurar_descriptores_originales();
}

void ejecucion_exit(tline *line) {
	guardar_descriptores_originales(); //para que se pueda redirigir la salida estándar/error con exit
	if (line->redirect_output != NULL) {
		redireccion_salida(line->redirect_output);
	}
	if (line->redirect_error != NULL) {
		redireccion_error(line->redirect_error);
	}

	printf("Saliendo de la minishell\n");
	matar_background(); //mato procesos background antes de salir de la minishell
	restaurar_descriptores_originales();
	exit(0);
}

void ejecucion_umask(tline *line) {
	mode_t mascara_actual;
	mode_t mascara;
	int octal;

	guardar_descriptores_originales(); //para que se pueda redirigir la salida estándar/error con umask
	if (line->redirect_output != NULL) {
		redireccion_salida(line->redirect_output);
	}
	if (line->redirect_error != NULL) {
		redireccion_error(line->redirect_error);
	}

	if (line->commands[0].argc == 1) { //para mostrar la máscara actual
        mascara_actual = umask(0);
        umask(mascara_actual);
        printf("%04o\n", mascara_actual);
    }
    else if (line->commands[0].argc == 2) { //para cambiar la máscara actual
        if (es_octal(line->commands[0].argv[1])) {
            octal = strtol(line->commands[0].argv[1], NULL, 8);
            mascara = (mode_t) octal;
            umask(mascara);
            printf("La máscara de modo se ha establecido en %#o\n", mascara);
        }
        else {
            fprintf(stderr, "Error: Modo octal no válido\n");
        }
    }
    else {
        fprintf(stderr, "Error: Modo octal no válido\n");
    }
    restaurar_descriptores_originales();
}

void ejecucion_fg(int pid, tline *line) {
    int encontrado = -1;
    int i;
    int status;

    guardar_descriptores_originales(); //para que se pueda redirigir la salida estándar/error con fg
	if (line->redirect_output != NULL) {
		redireccion_salida(line->redirect_output);
	}
	if (line->redirect_error != NULL) {
		redireccion_error(line->redirect_error);
	}

    if (array_background[0] < 0) {
        printf("No hay procesos en background para llevar a foreground.\n");
        restaurar_descriptores_originales();
        return;
    }

    if (pid == -1) { //no se pasa un pid concreto -> ultimo proceso en background
        i = 0;
        while (array_background[i] != -101) {
            i++;
        }
        pid = array_background[i - 1];
        encontrado = i - 1;
        eliminar_pid_background(pid);
        printf("El mandato con pid %d ya ha sido pasado de background a foreground\n", pid);
    }
    else { //sí se pasa un pid concreto
        i = 0;
        while (array_background[i] != -101) { //busco el ID en el array de background
            if (array_background[i] == pid) {
                encontrado = i;
                eliminar_pid_background(pid);
                printf("El mandato con pid %d ya ha sido pasado de background a foreground\n", pid);
                break;
            }
            i++;
        }
    }

    if (encontrado == -1 || array_background[encontrado] == -1) {
        printf("El proceso con ID %d no se encuentra en background.\n", pid);
        restaurar_descriptores_originales();
        return;
    }

    signal(SIGINT, manejar_sigint);

    //espero a que el proceso en background termine
    waitpid(pid, &status, 0);

    restaurar_descriptores_originales();

    //redirijo la entrada y salida estándar
    dup2(STDIN_FILENO, fileno(stdin));
    dup2(STDOUT_FILENO, fileno(stdout));
}

int ejecucion_cd(tline *line) {
	char *dir;
	char buffer[512];

	guardar_descriptores_originales(); //para que se pueda redirigir la salida estándar/error con cd
	if (line->redirect_output != NULL) {
		redireccion_salida(line->redirect_output);
	}
	if (line->redirect_error != NULL) {
		redireccion_error(line->redirect_error);
	}

	if(line->commands[0].argc > 2) {
		fprintf(stderr,"Uso: %s directorio\n", line->commands[0].argv[0]);
		restaurar_descriptores_originales();
	  	return 1;
	}

	if (line->commands[0].argc == 1) { //no argumentos, entonces con HOME
		dir = getenv("HOME");
		if (dir == NULL) {
		  	fprintf(stderr,"No existe la variable $HOME\n");
		}
	}
	else { //sí argumentos
		dir = line->commands[0].argv[1];
	}

	//compruebo si es un directorio
	if (chdir(dir) != 0) {
		fprintf(stderr,"Error al cambiar de directorio: %s\n", strerror(errno));
	}
	printf( "El directorio actual es: %s\n", getcwd(buffer, sizeof(buffer)));

	restaurar_descriptores_originales();
	return 0;
}

void un_comando(tline *line, char buf[1024]) {
	pid_t pid = fork();
	int status;

    //ERROR FORK
    if (pid < 0) {
            fprintf(stderr, "Falló el fork. \n%s\n", strerror(errno));
            return;
    }
	//HIJO
	else if (pid == 0) {
		if (line->background == 1) { //BG
			signal(SIGINT, SIG_IGN); //el ^C se ignora
		}
		else { //FG
			signal(SIGINT, manejar_sigint);
		}

		if (line->redirect_input != NULL) {
			redireccion_entrada(line->redirect_input);
		}
		if (line->redirect_output != NULL) {
			redireccion_salida(line->redirect_output);
		}
		if (line->redirect_error != NULL) {
			redireccion_error(line->redirect_error);
		}
        execvp(line->commands[0].filename, line->commands[0].argv);
        //si la ejecución del comando da error:
        fprintf(stderr, "Error al ejecutar el comando: %s\n", strerror(errno));
        return;
    }
	//PADRE
    else {
        if (line->background == 1) { //BG
            signal(SIGINT, SIG_IGN); //el ^C se ignora
            printf("[%d]\n", pid);
            waitpid(pid, &status, WNOHANG);
            agregar_pid_background(pid, buf);
        }
        else { //FG
            signal(SIGINT, manejar_sigint);
            waitpid(pid, NULL, 0);
        }
    }
}

void varios_comandos(tline *line) {
	pid_t pid;
	int status;
	int i, j;
	int **pipes = (int**) malloc ((line->ncommands-1) * sizeof(int*));
	pid_t *pids = (pid_t*) malloc(line->ncommands * sizeof(pid_t));

	for (i = 0; i < (line->ncommands-1); i++) {
        pipes[i] = (int*) malloc (2 * sizeof(int));
	}

	//creo todos los pipes
	for (i = 0; i < line->ncommands-1; i++) {
		if (pipe(pipes[i]) == -1) {
            fprintf(stderr, "Fallo al crear el pipe: %s\n", strerror(errno));
            exit(EXIT_FAILURE);
        }
	}
	for (i = 0; i < line->ncommands; i++) {
		pid = fork();
		//ERROR FORK
		if(pid < 0) {
			fprintf(stderr, "Falló el fork().\n%s\n", strerror(errno));
			return;
		}
		//HIJOS
		else if (pid == 0) {
			if (line->background == 1) { //BG
				signal(SIGINT, SIG_IGN); //el ^C se ignora
			}
			else { //FG
				signal(SIGINT, manejar_sigint);
			}
			if(i == 0) { //1ER HIJO
				if (line->redirect_input != NULL) {
					redireccion_entrada(line->redirect_input);
				}
				for (j = 0; j < line->ncommands-1; j++) { //cierro la entrada y la salida de los pipes que no va a utilizar ese hijo
                    if (j != i) {
                        close(pipes[j][1]);
                        close(pipes[j][0]);
                    }
                }

				close(pipes[i][0]); //cierro la lectura del pipe
				dup2(pipes[i][1],1);
				close(pipes[i][1]); //cierro la escritura del pipe

				execvp(line->commands[i].argv[0], line->commands[i].argv);
                //si la ejecución del comando da error:
                fprintf(stderr, "Error al ejecutar el comando: %s\n", strerror(errno));
                return;
			}
			else if(i == (line->ncommands-1)) { //ULTIMO HIJO
				if (line->redirect_output != NULL) {
					redireccion_salida(line->redirect_output);
				}
				if (line->redirect_error != NULL) {
					redireccion_error(line->redirect_error);
				}
				for (j = 0; j < line->ncommands-1; j++) { //cierro la entrada y la salida de los pipes que no va a utilizar ese hijo
					if (j != i - 1) {
                        close(pipes[j][0]);
					}
					close(pipes[j][1]);
                }

				close(pipes[i-1][1]); //cierro la escritura del pipe anterior
				dup2(pipes[i-1][0],0);
				close(pipes[i-1][0]); //cierro la lectura del pipe anterior

				execvp(line->commands[i].argv[0], line->commands[i].argv);
                //si la ejecución del comando da error:
                fprintf(stderr, "Error al ejecutar el comando: %s\n", strerror(errno));
                return;
			}
			else { //RESTO HIJOS
				for (j = 0; j < line->ncommands-1; j++) { //cierro la entrada y la salida de los pipes que no va a utilizar ese hijo
                    if (j != i && j != i - 1) {
                        close(pipes[j][0]);
                        close(pipes[j][1]);
                    }
                }

				close(pipes[i][0]); //cierro la lectura del pipe
				close(pipes[i-1][1]); //cierro la escritura del pipe anterior
				dup2(pipes[i-1][0],0);
				dup2(pipes[i][1],1);
				close(pipes[i][1]); //cierro la escritura del pipe
				close(pipes[i-1][0]); //cierro la lectura del pipe anterior

				execvp(line->commands[i].argv[0], line->commands[i].argv);
                //si la ejecución del comando da error:
                fprintf(stderr, "Error al ejecutar el comando: %s\n", strerror(errno));
                return;
			}
		}
		//PADRE
		else {
			if (line->background == 1) { //BG
				signal(SIGINT, SIG_IGN); //el ^C se ignora
			}
			else { //FG
				signal(SIGINT, manejar_sigint);
			}
			pids[i] = pid; //añado el pid del proceso al array de pids
		}
	}

	//cierro todos los pipes
	for (i = 0; i < line->ncommands - 1; i++) {
		close(pipes[i][0]);
		close(pipes[i][1]);
	}

    //espero a que terminen todos los hijos
	for (i = 0; i < line->ncommands; i++) {
        waitpid(pids[i], &status, 0);
    }

	//libero la memoria que he reservado con malloc
    free(pipes);
    free(pids);
}
