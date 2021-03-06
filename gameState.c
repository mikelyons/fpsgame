#include "gameState.h"
#include <stdio.h>
#include <math.h>
#include <float.h>
#include <stdint.h>
#include <SDL.h>
#include <GL/glew.h>
#include "pngloader.h"
#include "image.h"
#include "label.h"
#include "glUtil.h"

#define MOUSE_SENSITIVITY 0.006f
#define MOVEMENT_SPEED .02f
#define TURNING_TIME 1500.0f
#define DYING_TIME 3000.0f

/**
 * Returns whether the spheres are colliding.
 * @param movevec The relative velocity vector.
 */
static int isSphereCollision(VECTOR pos0, VECTOR pos1, float radius0, float radius1, VECTOR movevec) {
	// The vector from the center of the moving sphere to the center of the stationary
	VECTOR c = VectorSubtract(pos1, pos0);
	float radiiSum = radius0 + radius1;

	// Early escape test
	if (Vector3Length(movevec) < Vector3Length(c) - radiiSum) return 0;

	// Normalize movevec; check for zero vector
	VECTOR n = (VectorEqual(movevec, VectorReplicate(0.0f)) & 0x7) == 0x7 ? movevec : Vector4Normalize(movevec);
	float d = Vector3Dot(n, c);
	// Make sure the spheres are moving towards each other
	if (d < 0) return 0;

	float f = Vector3Dot(c, c) - d * d;
	float radiiSumSquared = radiiSum * radiiSum;
	if (f >= radiiSumSquared) return 0;

	float t = radiiSumSquared - f;
	if (t < 0) return 0;

	float distance = d - sqrt(t);
	float mag = Vector3Length(movevec);
	return mag >= distance;
}

static void processEnemies(struct GameState *gameState, float dt) {
	struct EntityManager *manager = &gameState->manager;
	VECTOR playerPos = manager->positions[gameState->player].position;
	const unsigned mask = POSITION_COMPONENT_MASK | VELOCITY_COMPONENT_MASK | ENEMY_COMPONENT_MASK;
	for (int i = 0; i < MAX_ENTITIES; ++i) {
		if ((manager->entityMasks[i] & mask) == mask) {
			VECTOR pos = manager->positions[i].position;
			VECTOR toward = VectorSubtract(playerPos, pos);
			if ((VectorEqual(toward, VectorReplicate(0.0f)) & 0x7) == 0x7) {
				manager->velocities[i] = VectorReplicate(0.0f);
			} else {
				manager->velocities[i] = VectorDivide(Vector4Normalize(toward), VectorReplicate(100.0f));
			}
		}
	}
}

static void processVelocities(struct GameState *gameState, float dt) {
	struct EntityManager *manager = &gameState->manager;
	const unsigned mask = POSITION_COMPONENT_MASK | VELOCITY_COMPONENT_MASK;
	for (int i = 0; i < MAX_ENTITIES; ++i) {
		if ((manager->entityMasks[i] & mask) == mask) {
			manager->positions[i].position = VectorAdd(manager->positions[i].position,
					VectorMultiply(VectorReplicate(dt), manager->velocities[i]));
		}
	}
}

static void processCollisions(struct GameState *gameState, float dt) {
	struct EntityManager *manager = &gameState->manager;
	const unsigned mask = POSITION_COMPONENT_MASK | VELOCITY_COMPONENT_MASK | COLLIDER_COMPONENT_MASK;
	for (int i = 0; i < MAX_ENTITIES && i == gameState->player; ++i) {
		if ((manager->entityMasks[i] & mask) == mask) {
			VECTOR pos0 = manager->positions[i].position;
			VECTOR velocity0 = manager->velocities[i];
			float radius0 = manager->colliders[i].radius;

			for (int j = i + 1; j < MAX_ENTITIES; ++j) {
				if ((manager->entityMasks[j] & mask) == mask) {
					VECTOR pos1 = manager->positions[j].position;
					VECTOR velocity1 = manager->velocities[j];
					float radius1 = manager->colliders[j].radius;

					VECTOR movevec = VectorMultiply(VectorReplicate(dt), VectorSubtract(velocity0, velocity1));
					if (isSphereCollision(pos0, pos1, radius0, radius1, movevec)) {
						printf("hello collisions %f\n", dt);
						gameState->playerData.dead = 1;
					}
				}
			}
		}
	}
}

static float getTurningFactor(float turn) {
	float x = turn / TURNING_TIME;
	x = sin(0.5f * M_PI * x);
	return x;
}

static float calcDyingEffectFactor(float timer) {
	float t = timer >= DYING_TIME ? 1.0f : timer / DYING_TIME;
	return cubicBezier(0.0f, 0.07f, 0.59f, 1.0f, t);
}

static void gameStateUpdate(struct State *state, float dt) {
	struct GameState *gameState = (struct GameState *) state;
	struct EntityManager *manager = &gameState->manager;
	const Uint8 *keys = SDL_GetKeyboardState(NULL);

	float timeScale = 1.0f;
	if (gameState->playerData.dead) {
		timeScale = 0.0f;
		gameState->playerData.deadTimer += dt;
	}
	dt *= timeScale;

	if (gameState->noclip) {
		int x, y;
		Uint32 button = SDL_GetRelativeMouseState(&x, &y);
		gameState->yaw -= x * MOUSE_SENSITIVITY;
		gameState->pitch -= y * MOUSE_SENSITIVITY;
	} else {
		// Handle turning
		if (keys[SDL_SCANCODE_A] ^ keys[SDL_SCANCODE_D]) {
			gameState->playerData.turn += keys[SDL_SCANCODE_A] ? -dt : dt;
		} else {
			if (fabs(gameState->playerData.turn) < TURNING_TIME / 3.0f) {
				if (gameState->playerData.turn < -dt) gameState->playerData.turn += dt;
				else if (gameState->playerData.turn > dt) gameState->playerData.turn -= dt;
				else gameState->playerData.turn = 0;
			}
		}
		if (gameState->playerData.turn < -TURNING_TIME) gameState->playerData.turn = -TURNING_TIME;
		else if (gameState->playerData.turn > TURNING_TIME) gameState->playerData.turn = TURNING_TIME;
		gameState->yaw -= 0.006f * getTurningFactor(gameState->playerData.turn) * dt;
	}

	if (gameState->yaw > M_PI) gameState->yaw -= 2 * M_PI;
	else if (gameState->yaw < -M_PI) gameState->yaw += 2 * M_PI;
	// Clamp the pitch
	gameState->pitch = gameState->pitch < -M_PI / 2 ? -M_PI / 2 : gameState->pitch > M_PI / 2 ? M_PI / 2 : gameState->pitch;

	VECTOR forward = VectorSet(-MOVEMENT_SPEED * sin(gameState->yaw), 0, -MOVEMENT_SPEED * cos(gameState->yaw), 0),
		   up = VectorSet(0, 1, 0, 0),
		   right = VectorCross(forward, up);

	if (gameState->noclip) {
		VECTOR displacement = VectorReplicate(0.0f);
		if (keys[SDL_SCANCODE_W]) displacement = VectorAdd(displacement, forward);
		if (keys[SDL_SCANCODE_A]) displacement = VectorSubtract(displacement, right);
		if (keys[SDL_SCANCODE_S]) displacement = VectorSubtract(displacement, forward);
		if (keys[SDL_SCANCODE_D]) displacement = VectorAdd(displacement, right);
		if (keys[SDL_SCANCODE_SPACE]) displacement = VectorAdd(displacement, VectorSet(0, MOVEMENT_SPEED, 0, 0));
		if (keys[SDL_SCANCODE_LSHIFT]) displacement = VectorSubtract(displacement, VectorSet(0, MOVEMENT_SPEED, 0, 0));
		gameState->position = VectorAdd(gameState->position, VectorMultiply(VectorReplicate(dt), displacement));
	} else {
		manager->velocities[gameState->player] = keys[SDL_SCANCODE_W] ? forward : VectorReplicate(0.0f);
	}

	processEnemies(gameState, dt);
	if (!gameState->playerData.dead) {
		processCollisions(gameState, dt);
	}
	processVelocities(gameState, dt);
}

static void gameStateDraw(struct State *state, float dt) {
	struct GameState *gameState = (struct GameState *) state;
	struct SpriteBatch *batch = gameState->batch;
	struct EntityManager *manager = &gameState->manager;

	if (gameState->noclip) {
		rendererDraw(&gameState->renderer, gameState->position, gameState->yaw, gameState->pitch, 0.0f, dt);
	} else {
		VECTOR position = VectorAdd(manager->positions[gameState->player].position, VectorSet(0.0f, 1.4f, 0.0f, 0.0f));
		float yaw = gameState->yaw;
		float pitch = 0;
		float roll = M_PI / 9 * getTurningFactor(gameState->playerData.turn);
		if (gameState->playerData.dead) {
			float deadFactor = calcDyingEffectFactor(gameState->playerData.deadTimer);
			if (gameState->playerData.deadTimer > DYING_TIME / 2) yaw += 0.0002f * (gameState->playerData.deadTimer - DYING_TIME / 2);
			pitch = -M_PI / 7.0f * deadFactor;
			roll = (1.0f - deadFactor) * roll;
			position = VectorAdd(position, VectorSet(0.0f, deadFactor * 6.0f, 0.0f, 0.0f));
			position = VectorAdd(position, VectorMultiply(VectorReplicate(10.0f * deadFactor), VectorSet(cos(pitch) * sin(yaw), 0.0f, cos(pitch) * cos(yaw), 0.0f)));
		}
		rendererDraw(&gameState->renderer, position, yaw, pitch, roll, dt);
	}

	// Draw GUI
	spriteBatchBegin(batch);
	// widgetValidate(gameState->flexLayout, 800, 600);
	// widgetDraw(gameState->flexLayout, batch);
	spriteBatchEnd(batch);
}

static void gameStateResize(struct State *state, int width, int height) {
	struct GameState *gameState = (struct GameState *) state;
	rendererResize(&gameState->renderer, width, height);
	widgetLayout(gameState->flexLayout, width, MEASURE_EXACTLY, height, MEASURE_EXACTLY);
}

static struct FlexParams params0 = { ALIGN_END, -1, 100, UNDEFINED, 20, 0, 20, 20 },
						 params2 = {ALIGN_CENTER, 1, 100, UNDEFINED, 0, 0, 0, 50},
						 params1 = { ALIGN_CENTER, 1, UNDEFINED, UNDEFINED, 0, 0, 0, 0 };

void gameStateInitialize(struct GameState *gameState, struct SpriteBatch *batch) {
	struct State *state = (struct State *) gameState;
	state->update = gameStateUpdate;
	state->draw = gameStateDraw;
	state->resize = gameStateResize;
	gameState->batch = batch;
	struct EntityManager *manager = &gameState->manager;
	entityManagerInit(manager);
	struct Renderer *renderer = &gameState->renderer;
	rendererInit(renderer, manager, 800, 600);

	gameState->position = VectorSet(0, 0, 0, 1);
	gameState->yaw = 0;
	gameState->pitch = 0;
	gameState->objModel = loadModelFromObj("assets/pyramid.obj");
	if (!gameState->objModel) {
		printf("Failed to load model.\n");
	}
	gameState->groundModel = loadModelFromObj("assets/ground.obj");
	if (!gameState->groundModel) {
		printf("Failed to load ground model.\n");
	}

	gameState->player = entityManagerSpawn(manager);
	manager->entityMasks[gameState->player] = POSITION_COMPONENT_MASK | VELOCITY_COMPONENT_MASK | COLLIDER_COMPONENT_MASK;
	manager->positions[gameState->player].position = VectorSet(0.0f, 0.0f, 0.0f, 1.0f);
	manager->velocities[gameState->player] = VectorReplicate(0.0f);
	manager->colliders[gameState->player].radius = 0.2f;

	Entity ground = entityManagerSpawn(manager);
	manager->entityMasks[ground] = POSITION_COMPONENT_MASK | MODEL_COMPONENT_MASK;
	manager->positions[ground].position = VectorSet(0.0f, 0.0f, 0.0f, 1.0f);
	manager->models[ground].model = gameState->groundModel;

	const float range = 400.0f;
	for (int i = 0; i < 35; ++i) {
		Entity enemy = entityManagerSpawn(manager);
		manager->entityMasks[enemy] = POSITION_COMPONENT_MASK | MODEL_COMPONENT_MASK | VELOCITY_COMPONENT_MASK | COLLIDER_COMPONENT_MASK | ENEMY_COMPONENT_MASK;
		manager->positions[enemy].position = VectorSet(range * randomFloat() - range / 2, 0.0f, range * randomFloat() - range / 2, 1.0f);
		manager->models[enemy].model = gameState->objModel;
		manager->velocities[enemy] = VectorReplicate(0.0f);
		manager->colliders[enemy].radius = 0.5f;
	}

	// Initialize GUI
	gameState->flexLayout = malloc(sizeof(struct FlexLayout));
	flexLayoutInitialize(gameState->flexLayout, DIRECTION_ROW, ALIGN_START);

	int width, height;
	gameState->cat = loadPngTexture("assets/cat.png", &width, &height);
	if (!gameState->cat) {
		fprintf(stderr, "Failed to load png image.\n");
	}
	gameState->image0 = malloc(sizeof(struct Image));
	imageInitialize(gameState->image0, gameState->cat, width, height, 0);
	gameState->image0->layoutParams = &params0;
	containerAddChild(gameState->flexLayout, gameState->image0);

	gameState->image1 = malloc(sizeof(struct Image));
	imageInitialize(gameState->image1, gameState->cat, width, height, 0);
	gameState->image1->layoutParams = &params1;
	containerAddChild(gameState->flexLayout, gameState->image1);

	gameState->font = loadFont("assets/DejaVuSans.ttf", 512, 512);
	if (!gameState->font) {
		printf("Could not load font.");
	}

	gameState->label = labelNew(gameState->font, "Axel ffi! and the AV. HHHHHHHH Hi! (215): tv-hund. fesflhslg");
	gameState->label->layoutParams = &params2;
	containerAddChild(gameState->flexLayout, gameState->label);

	gameState->noclip = 0;

	gameState->playerData.turn = 0.0f;
	gameState->playerData.dead = 0;
	gameState->playerData.deadTimer = 0.0f;
}

void gameStateDestroy(struct GameState *gameState) {
	rendererDestroy(&gameState->renderer);
	destroyModel(gameState->objModel);

	fontDestroy(gameState->font);
	containerDestroy(gameState->flexLayout);
	free(gameState->flexLayout);
	free(gameState->image0);
	free(gameState->image1);
	labelDestroy(gameState->label);
	free(gameState->label);
	glDeleteTextures(1, &gameState->cat);
}
